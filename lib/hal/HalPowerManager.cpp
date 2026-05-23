#include "HalPowerManager.h"

#include <Logging.h>
#include <WiFi.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include <cassert>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

void HalPowerManager::begin() {
  pinMode(BAT_GPIO4, INPUT);
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Note: We don't use mutex here to avoid too much overhead,
  // it's not very important if we read a slightly stale value for currentLockMode
  const LockMode mode = currentLockMode;

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  if (gpio.deviceIsX3()) {
#ifdef ENABLE_SERIAL_LOG
    // Tear down HWCDC so the host sees a clean disconnect and the peripheral
    // doesn't hold power domains that interfere with USB-powered GPIO wake.
    // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
    // (which doesn't expose end()).
    logSerial.end();
#endif

    // Pre-sleep routines from the original firmware
    // GPIO13 is connected to battery latch MOSFET, we need to make sure it's low during sleep
    // Note that this means the MCU will be completely powered off during sleep, including RTC
    constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;
    gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_SPIWP, 0);
    esp_sleep_config_gpio_isolate();
    gpio_deep_sleep_hold_en();
    gpio_hold_en(GPIO_SPIWP);
    pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
    // Arm the wakeup trigger *after* the button is released
    // Note: this is only useful for waking up on USB power. On battery, the MCU will be completely powered off, so the
    // power button is hard-wired to briefly provide power to the MCU, waking it up regardless of the wakeup source
    // configuration
    esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  } else {
#endif
    // Power button is ACTIVE-HIGH: resting = 0, pressed = 1.
    // Enable pulldown so the pin is firmly LOW at rest (no spurious wakeup).
    rtc_gpio_pulldown_en(static_cast<gpio_num_t>(InputManager::POWER_BUTTON_PIN));
    // Hold the RTC GPIO state (including pulldown) through deep sleep.
    rtc_gpio_hold_en(static_cast<gpio_num_t>(InputManager::POWER_BUTTON_PIN));
    // Wake when the button goes HIGH (ANY_HIGH).
    esp_sleep_enable_ext1_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_HIGH);
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  }
#endif
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO4, 2.0f, BAT_CHECK);
  return battery.readPercentage();
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
