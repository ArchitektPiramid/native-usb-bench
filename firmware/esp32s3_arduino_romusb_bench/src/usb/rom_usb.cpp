#include "rom_usb.h"
#include "rom_usb_internal.h"
#include "descriptors.h"
#include <Arduino.h>
#include "esp32s3/rom/usb/cdc_acm.h"
#include "esp_intr_alloc.h"
#include "soc/interrupts.h"
#include "soc/soc.h"
#include "hal/usb_wrap_ll.h"
#include "hal/usb_dwc_ll.h"
#include "esp32s3/rom/usb/usb_os_glue.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace RomUsb {
namespace Internal {
volatile bool connected = false;
bool usb_started = false;
namespace {
intr_handle_t usb_interrupt = nullptr;
portMUX_TYPE usb_lock = portMUX_INITIALIZER_UNLOCKED;
int init_error = 0;

// ROM USB requires OS callbacks before usb_enable().
void usbDisableInterrupts() { portENTER_CRITICAL_SAFE(&usb_lock); }
void usbEnableInterrupts() { portEXIT_CRITICAL_SAFE(&usb_lock); }
int usbWait(int delay_us) {
  if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING || xPortInIsrContext()) {
    if (delay_us > 0) esp_rom_delay_us(delay_us);
    return delay_us > 0 ? delay_us : 1;
  }
  if (delay_us <= 0) { taskYIELD(); return 1; }
  if (delay_us < 1000) { esp_rom_delay_us(delay_us); return delay_us; }
  const TickType_t ticks = pdMS_TO_TICKS((delay_us + 999) / 1000);
  vTaskDelay(ticks ? ticks : 1);
  return (ticks ? ticks : 1) * portTICK_PERIOD_MS * 1000;
}

void usbIsr(void*) {
  usb_dc_check_poll_for_interrupts();
}
} // namespace

// Track Chapter 9 configuration; reset Bulk ownership on bus loss.
void interfaceConfigured(uint8_t) { connected = true; }
void statusCallback(usb_dc_status_code status, uint8_t*) {
  if (status == USB_DC_RESET || status == USB_DC_DISCONNECTED) {
    connected = false;
    resetBulkState();
  }
  if (status == USB_DC_CONFIGURED) {
    connected = true;
    invalidateBulkEndpoint();
  }
}

static usb_ep_cfg_data endpoints[] = {
    {inCallback, kEpIn},
    {outCallback, kEpOut},
};
// ROM must retain these buffers and callbacks for the device lifetime.
static uint8_t ep0_payload[256] __attribute__((aligned(4)));
static uint8_t ep0_vendor[256] __attribute__((aligned(4)));
static usb_cfg_data usb_config = {
    RomUsbDescriptors::kDescriptors,
    RomUsbDescriptors::kDescriptors + RomUsbDescriptors::kInterfaceOffset,
    interfaceConfigured,
    statusCallback,
    {nullptr, vendorRequest, customRequest, ep0_payload, ep0_vendor},
    2,
    endpoints,
};
} // namespace Internal

bool begin() {
  using namespace Internal;
  // Restore ROM USB ownership after any bootloader CDC session.
  usb_dev_deinit();
  usb_dw_ctrl_deinit();
  uart_acm_dev = nullptr;
  rom_usb_osglue.int_dis_proc = usbDisableInterrupts;
  rom_usb_osglue.int_ena_proc = usbEnableInterrupts;
  rom_usb_osglue.wait_proc = usbWait;

  usb_wrap_ll_phy_enable_pad(&USB_WRAP, true);
  usb_wrap_ll_phy_enable_external(&USB_WRAP, false);

  const esp_err_t irq_rc = esp_intr_alloc(
      ETS_USB_INTR_SOURCE, ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_INTRDISABLED,
      usbIsr, nullptr, &usb_interrupt);
  if (irq_rc != ESP_OK) {
    init_error = static_cast<int>(irq_rc);
#if NUB_ROMUSB_LOG_LEVEL >= 1
    Serial.printf("[ROMUSB] IRQ allocation failed: %d\n", init_error);
#endif
    return false;
  }
  // Both calls are necessary: the first installs descriptors/EP0 handlers,
  // the second attaches the USB controller to the bus.
  init_error = usb_set_config(&usb_config);
  if (init_error != 0) {
#if NUB_ROMUSB_LOG_LEVEL >= 1
    Serial.printf("[ROMUSB] usb_set_config failed: %d\n", init_error);
#endif
    esp_intr_free(usb_interrupt);
    usb_interrupt = nullptr;
    return false;
  }
  init_error = usb_enable(&usb_config);
  if (init_error != 0) {
#if NUB_ROMUSB_LOG_LEVEL >= 1
    Serial.printf("[ROMUSB] usb_enable failed: %d\n", init_error);
#endif
    usb_deconfig();
    esp_intr_free(usb_interrupt);
    usb_interrupt = nullptr;
    return false;
  }
  usb_dc_check_poll_for_interrupts();
  usb_dwc_ll_gahbcfg_en_global_intr(USB_DWC_LL_GET_HW(0));
  const esp_err_t enable_rc = esp_intr_enable(usb_interrupt);
  if (enable_rc != ESP_OK) {
    init_error = static_cast<int>(enable_rc);
#if NUB_ROMUSB_LOG_LEVEL >= 1
    Serial.printf("[ROMUSB] IRQ enable failed: %d\n", init_error);
#endif
    usb_disable();
    usb_deconfig();
    esp_intr_free(usb_interrupt);
    usb_interrupt = nullptr;
    return false;
  }
  usb_started = true;
#if NUB_ROMUSB_LOG_LEVEL >= 2
  Serial.println("[ROMUSB] Ready");
#endif
  return true;
}

void task() {
  if (Internal::usb_started && Internal::connected) Internal::ensureBulkInEndpoint();
  Internal::serviceBulkIn();
  Internal::bulkDiagnosticTick();
}
bool configured() { return Internal::usb_started && Internal::connected; }
} // namespace RomUsb
