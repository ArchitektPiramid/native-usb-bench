//! Pipelined libusb Bulk IN reads; only the USB worker pumps completion events.
//! Transfers and their buffers remain allocated until every cancellation callback returns.

use rusb::{ffi, GlobalContext, UsbContext};
use std::{
    ffi::c_void,
    ptr::NonNull,
    sync::atomic::{AtomicBool, AtomicI32, Ordering},
    time::{Duration, Instant},
};

// Full-Speed keeps the original low-memory read queue. HS targets receive
// larger submitted URBs so the controller almost always has pending IN tokens.
const FS_READ_BYTES: usize = 8192;
const FS_READ_DEPTH: usize = 4;
const HS_READ_BYTES: usize = 64 * 1024;
const HS_READ_DEPTH: usize = 16;
const READ_TIMEOUT_MS: u32 = 250;

struct Slot {
    transfer: NonNull<ffi::libusb_transfer>,
    data: Box<[u8]>,
    complete: AtomicBool,
    status: AtomicI32,
    actual: AtomicI32,
    in_flight: bool,
    ticket: u64,
}

// libusb invokes this from event handling. No parsing, logging or USB calls here.
extern "system" fn on_transfer(transfer: *mut ffi::libusb_transfer) {
    unsafe {
        let slot = &*((*transfer).user_data as *const Slot);
        slot.status.store((*transfer).status, Ordering::Relaxed);
        slot.actual.store((*transfer).actual_length, Ordering::Relaxed);
        slot.complete.store(true, Ordering::Release);
    }
}

pub(crate) struct AsyncBulkReader {
    slots: Vec<Box<Slot>>,
    handle: *mut ffi::libusb_device_handle,
    endpoint: u8,
    next_ticket: u64,
    next_to_parse: u64,
    active: bool,
}

impl AsyncBulkReader {
    pub(crate) fn new(
        handle: &rusb::DeviceHandle<GlobalContext>,
        endpoint: u8,
        high_speed: bool,
    ) -> Result<Self, String> {
        let (depth, read_bytes) = if high_speed {
            (HS_READ_DEPTH, HS_READ_BYTES)
        } else {
            (FS_READ_DEPTH, FS_READ_BYTES)
        };
        let mut reader = Self {
            slots: Vec::with_capacity(depth),
            handle: handle.as_raw(),
            endpoint,
            next_ticket: 0,
            next_to_parse: 0,
            active: false,
        };
        for _ in 0..depth {
            let raw = NonNull::new(unsafe { ffi::libusb_alloc_transfer(0) })
                .ok_or_else(|| "Unable to allocate a libusb transfer".to_string())?;
            let slot = Box::new(Slot {
                transfer: raw,
                data: vec![0; read_bytes].into_boxed_slice(),
                complete: AtomicBool::new(false),
                status: AtomicI32::new(0),
                actual: AtomicI32::new(0),
                in_flight: false,
                ticket: 0,
            });
            reader.slots.push(slot);
        }
        Ok(reader)
    }

    pub(crate) fn start(&mut self) -> Result<(), String> {
        if self.active { return Ok(()); }
        self.next_ticket = 0;
        self.next_to_parse = 0;
        self.active = true;
        for i in 0..self.slots.len() {
            if let Err(error) = self.submit(i) {
                self.cancel_and_drain();
                return Err(error);
            }
        }
        Ok(())
    }

    fn submit(&mut self, index: usize) -> Result<(), String> {
        let slot = &mut self.slots[index];
        debug_assert!(!slot.in_flight);
        slot.ticket = self.next_ticket;
        self.next_ticket += 1;
        slot.complete.store(false, Ordering::Relaxed);
        slot.actual.store(0, Ordering::Relaxed);
        unsafe {
            ffi::libusb_fill_bulk_transfer(
                slot.transfer.as_ptr(), self.handle, self.endpoint,
                slot.data.as_mut_ptr(), slot.data.len() as i32,
                on_transfer, (&mut **slot as *mut Slot).cast::<c_void>(), READ_TIMEOUT_MS,
            );
            let rc = ffi::libusb_submit_transfer(slot.transfer.as_ptr());
            if rc != 0 { return Err(format!("libusb_submit_transfer failed: {rc}")); }
        }
        slot.in_flight = true;
        Ok(())
    }

    /// Parse completions in USB submission order, preserving NUB1 byte order.
    pub(crate) fn poll(&mut self, timeout: Duration, mut consume: impl FnMut(&[u8])) -> Result<u64, String> {
        if !self.active { return Ok(0); }
        GlobalContext::default().handle_events(Some(timeout))
            .map_err(|e| format!("libusb async event loop: {e}"))?;
        let mut reads = 0;
        loop {
            let Some(index) = self.slots.iter().position(|slot| slot.in_flight
                && slot.ticket == self.next_to_parse
                && slot.complete.load(Ordering::Acquire)) else { break };
            let slot = &mut self.slots[index];
            let status = slot.status.load(Ordering::Relaxed);
            let actual = slot.actual.load(Ordering::Relaxed).max(0) as usize;
            slot.in_flight = false;
            self.next_to_parse += 1;
            // libusb can report partial data alongside a timeout.
            if actual > 0 && (status == ffi::constants::LIBUSB_TRANSFER_COMPLETED
                || status == ffi::constants::LIBUSB_TRANSFER_TIMED_OUT) {
                consume(&slot.data[..actual.min(slot.data.len())]);
                reads += 1;
            }
            if status != ffi::constants::LIBUSB_TRANSFER_COMPLETED
                && status != ffi::constants::LIBUSB_TRANSFER_TIMED_OUT {
                self.cancel_and_drain();
                return Err(format!("Bulk IN async transfer status: {status}"));
            }
            self.submit(index)?;
        }
        Ok(reads)
    }

    /// Collect completed USB reads in submission order after firmware STOP.
    /// Incomplete/cancelled transfers are deliberately not parsed as full frames.
    pub(crate) fn stop_and_collect(&mut self, mut consume: impl FnMut(&[u8])) -> u64 {
        if !self.active { return 0; }
        self.active = false;
        // Give already-issued reads a short window to finish before cancellation.
        for _ in 0..4 {
            let _ = GlobalContext::default().handle_events(Some(Duration::from_millis(10)));
        }
        self.cancel_and_drain();
        let mut ordered: Vec<usize> = (0..self.slots.len()).collect();
        ordered.sort_by_key(|&idx| self.slots[idx].ticket);
        let mut expected = self.next_to_parse;
        let mut reads = 0;
        for index in ordered {
            let slot = &self.slots[index];
            if slot.ticket != expected || !slot.complete.load(Ordering::Acquire) { break; }
            let status = slot.status.load(Ordering::Relaxed);
            if status != ffi::constants::LIBUSB_TRANSFER_COMPLETED
                && status != ffi::constants::LIBUSB_TRANSFER_TIMED_OUT { break; }
            let actual = slot.actual.load(Ordering::Relaxed).max(0) as usize;
            if actual != 0 {
                consume(&slot.data[..actual.min(slot.data.len())]);
                reads += 1;
            }
            expected += 1;
        }
        reads
    }

    pub(crate) fn cancel_and_drain(&mut self) {
        self.active = false;
        for slot in &self.slots {
            if slot.in_flight && !slot.complete.load(Ordering::Acquire) {
                unsafe { ffi::libusb_cancel_transfer(slot.transfer.as_ptr()); }
            }
        }
        // A cancelled transfer must complete via a callback before it may be freed.
        let deadline = Instant::now() + Duration::from_secs(3);
        while self.slots.iter().any(|slot| slot.in_flight && !slot.complete.load(Ordering::Acquire))
            && Instant::now() < deadline {
            let _ = GlobalContext::default().handle_events(Some(Duration::from_millis(20)));
        }
        for slot in &mut self.slots {
            if slot.complete.load(Ordering::Acquire) { slot.in_flight = false; }
        }
    }
}

impl Drop for AsyncBulkReader {
    fn drop(&mut self) {
        self.cancel_and_drain();
        for slot in self.slots.drain(..) {
            if slot.in_flight {
                // Never free a buffer that libusb could still access.
                // This abnormal path leaks one slot instead of causing use-after-free.
                let _ = Box::into_raw(slot);
            } else {
                unsafe { ffi::libusb_free_transfer(slot.transfer.as_ptr()); }
            }
        }
    }
}
