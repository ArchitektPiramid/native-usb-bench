use serde::Serialize;
use std::sync::Mutex;
use std::time::Instant;

#[derive(Debug, Clone, Serialize)]
pub struct ProcessStats {
    /// CPU usage of the NativeUSB Bench host process, normalized to the
    /// machine's total logical CPU capacity (Task Manager-style 0..100%).
    pub cpu_usage_percent: Option<f32>,
}

pub struct ProcessStatsState {
    sampler: Mutex<ProcessCpuSampler>,
}

impl ProcessStatsState {
    pub fn new() -> Self {
        Self {
            sampler: Mutex::new(ProcessCpuSampler::new()),
        }
    }

    pub fn sample(&self) -> Result<ProcessStats, String> {
        let mut sampler = self
            .sampler
            .lock()
            .map_err(|_| "process CPU sampler mutex poisoned".to_string())?;

        Ok(sampler.sample())
    }
}

struct ProcessCpuSampler {
    last_wall: Instant,
    last_cpu_time_100ns: Option<u64>,
    logical_cpu_count: usize,
}

impl ProcessCpuSampler {
    fn new() -> Self {
        Self {
            last_wall: Instant::now(),
            last_cpu_time_100ns: read_process_cpu_time_100ns(),
            logical_cpu_count: std::thread::available_parallelism()
                .map(|count| count.get())
                .unwrap_or(1),
        }
    }

    fn sample(&mut self) -> ProcessStats {
        let wall_now = Instant::now();
        let cpu_now = read_process_cpu_time_100ns();
        let wall_delta = wall_now.duration_since(self.last_wall);

        let cpu_usage_percent = match (self.last_cpu_time_100ns, cpu_now) {
            (Some(previous), Some(current)) if current >= previous && !wall_delta.is_zero() => {
                let process_cpu_seconds = (current - previous) as f64 / 10_000_000.0;
                let total_cpu_seconds =
                    wall_delta.as_secs_f64() * self.logical_cpu_count.max(1) as f64;

                if total_cpu_seconds > 0.0 {
                    Some(((process_cpu_seconds / total_cpu_seconds) * 100.0).clamp(0.0, 100.0) as f32)
                } else {
                    Some(0.0)
                }
            }
            _ => None,
        };

        self.last_wall = wall_now;
        self.last_cpu_time_100ns = cpu_now;

        ProcessStats { cpu_usage_percent }
    }
}

#[cfg(target_os = "windows")]
fn read_process_cpu_time_100ns() -> Option<u64> {
    use std::ffi::c_void;

    #[repr(C)]
    #[derive(Clone, Copy, Default)]
    struct FileTime {
        low_date_time: u32,
        high_date_time: u32,
    }

    impl FileTime {
        fn as_u64(self) -> u64 {
            ((self.high_date_time as u64) << 32) | self.low_date_time as u64
        }
    }

    #[link(name = "kernel32")]
    extern "system" {
        fn GetCurrentProcess() -> *mut c_void;
        fn GetProcessTimes(
            process: *mut c_void,
            creation_time: *mut FileTime,
            exit_time: *mut FileTime,
            kernel_time: *mut FileTime,
            user_time: *mut FileTime,
        ) -> i32;
    }

    let mut creation = FileTime::default();
    let mut exit = FileTime::default();
    let mut kernel = FileTime::default();
    let mut user = FileTime::default();

    let ok = unsafe {
        GetProcessTimes(
            GetCurrentProcess(),
            &mut creation,
            &mut exit,
            &mut kernel,
            &mut user,
        )
    };

    if ok == 0 {
        None
    } else {
        Some(kernel.as_u64().saturating_add(user.as_u64()))
    }
}

#[cfg(not(target_os = "windows"))]
fn read_process_cpu_time_100ns() -> Option<u64> {
    // The current lightweight implementation intentionally uses the native
    // Windows process timer because Windows is the tested host platform.
    // Returning None keeps other targets buildable and makes the UI show "—".
    None
}
