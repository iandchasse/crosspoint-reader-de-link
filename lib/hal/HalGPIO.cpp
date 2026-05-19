#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <SPI.h>
#include <driver/usb_serial_jtag.h>

void HalGPIO::begin() {
  inputMgr.begin();
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);
  pinMode(BAT_CHECK, INPUT_PULLUP);  // MCP73832 STAT pin (open-drain)
}

void HalGPIO::update() {
  inputMgr.update();
  const bool connected = isUsbConnected();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

unsigned long HalGPIO::getPowerButtonHeldTime() const { return inputMgr.getPowerButtonHeldTime(); }

void HalGPIO::startDeepSleep() {
  powerManager.startDeepSleep(*this);
}

void HalGPIO::verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) {
  if (shortPressAllowed) {
    // Fast path - no duration check needed
    return;
  }
  // TODO: Intermittent edge case remains: a single tap followed by another single tap
  // can still power on the device. Tighten wake debounce/state handling here.

  // Calibrate: subtract boot time already elapsed, assuming button held since boot
  const uint16_t calibration = millis();
  const uint16_t calibratedDuration = (calibration < requiredDurationMs) ? (requiredDurationMs - calibration) : 1;

  const auto start = millis();
  inputMgr.update();
  // inputMgr.isPressed() may take up to ~500ms to return correct state
  while (!inputMgr.isPressed(BTN_POWER) && millis() - start < 1000) {
    delay(10);
    inputMgr.update();
  }
  if (inputMgr.isPressed(BTN_POWER)) {
    do {
      delay(10);
      inputMgr.update();
    } while (inputMgr.isPressed(BTN_POWER) && inputMgr.getPowerButtonHeldTime() < calibratedDuration);
    if (inputMgr.getPowerButtonHeldTime() < calibratedDuration) {
      startDeepSleep();
    }
  } else {
    startDeepSleep();
  }
}

bool HalGPIO::isUsbConnected() const {
  // Native ESP-IDF API: detects SOF packets from a connected USB host
  return usb_serial_jtag_is_connected();
}

bool HalGPIO::isCharging() const {
  // MCP73832 STAT pin (GPIO8): LOW = charging, Hi-Z = not charging/complete
  return digitalRead(BAT_CHECK) == LOW;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const bool usbConnected = isUsbConnected();
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  // Woke from deep sleep via GPIO (power button press)
  if (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP) {
    return WakeupReason::PowerButton;
  }

  // Cold power-on with no USB = battery-powered cold boot, treat as power button press
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) {
    return WakeupReason::PowerButton;
  }

  // Any non-sleep reset while USB is connected covers both:
  //   ESP_RST_UNKNOWN  = esptool default_reset / soft reset
  //   ESP_RST_POWERON  = esptool --after=hard_reset
  // In both cases we just got flashed (or USB was plugged in cold).
  // Return AfterFlash so we boot normally and keep the USB CDC port alive on Windows.
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && usbConnected) {
    return WakeupReason::AfterFlash;
  }

  return WakeupReason::Other;
}