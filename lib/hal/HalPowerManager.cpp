#include "HalPowerManager.h"

#include <Logging.h>
#include <WiFi.h>
#include <driver/rtc_io.h>
#include <esp_adc/adc_continuous.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_sleep.h>
#include <sys/time.h>
#include <ulp_common_defs.h>

#include <cassert>

#include "HalGPIO.h"
#include "RtcState.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include <esp32s3/ulp.h>
#else
#include <esp32/ulp.h>
#endif

#if defined(CONFIG_IDF_TARGET_ESP32S3)
extern "C" {
typedef struct {
  void (*fn)(void);
  void* arg;
} arduino_interrupt_config_t;

typedef struct {
  adc_oneshot_unit_handle_t adc_oneshot_handle;
  adc_continuous_handle_t adc_continuous_handle;
  arduino_interrupt_config_t adc_interrupt_handle;
  adc_cali_handle_t adc_cali_handle;
  uint32_t buffer_size;
  uint32_t conversion_frame_size;
} arduino_adc_handle_t;

extern arduino_adc_handle_t adc_handle[];
}
#endif

HalPowerManager powerManager;  // Singleton instance

namespace RtcState {
RTC_DATA_ATTR uint32_t sleepEntryTime = 0;
RTC_DATA_ATTR uint8_t pagesUntilFullRefreshRtc = 0;
}  // namespace RtcState

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

void HalPowerManager::startDeepSleep(HalGPIO& gpio, bool enableUlpWake, uint8_t pagesUntilFullRefresh) const {
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

#if defined(CONFIG_IDF_TARGET_ESP32S3)
  if (enableUlpWake) {
    LOG_DBG("PWR", "Configuring ULP FSM for button monitoring");

    // Release BAT pin from Arduino's ADC Oneshot driver
    pinMode(BAT_GPIO4, INPUT);

    // Deinitialize existing ADC1 oneshot unit held by Arduino core if in use,
    // to avoid "adc1 is already in use" conflict.
    if (adc_handle[ADC_UNIT_1].adc_oneshot_handle != nullptr) {
      adc_oneshot_del_unit(adc_handle[ADC_UNIT_1].adc_oneshot_handle);
      adc_handle[ADC_UNIT_1].adc_oneshot_handle = nullptr;
    }

    // Route BUTTON_ADC_PIN_1 and BUTTON_ADC_PIN_2 to the RTC IO MUX as analog inputs
    rtc_gpio_init(static_cast<gpio_num_t>(InputManager::BUTTON_ADC_PIN_1));
    rtc_gpio_set_direction(static_cast<gpio_num_t>(InputManager::BUTTON_ADC_PIN_1), RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_dis(static_cast<gpio_num_t>(InputManager::BUTTON_ADC_PIN_1));
    rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(InputManager::BUTTON_ADC_PIN_1));

    rtc_gpio_init(static_cast<gpio_num_t>(InputManager::BUTTON_ADC_PIN_2));
    rtc_gpio_set_direction(static_cast<gpio_num_t>(InputManager::BUTTON_ADC_PIN_2), RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_dis(static_cast<gpio_num_t>(InputManager::BUTTON_ADC_PIN_2));
    rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(InputManager::BUTTON_ADC_PIN_2));

    // Configure ADC1 channels 0 and 1 using the new oneshot driver
    adc_oneshot_unit_handle_t ulp_adc_handle = nullptr;
    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = ADC_UNIT_1;
    init_config.clk_src = ADC_RTC_CLK_SRC_DEFAULT;
    init_config.ulp_mode = ADC_ULP_MODE_FSM;
    adc_oneshot_new_unit(&init_config, &ulp_adc_handle);

    adc_oneshot_chan_cfg_t chan_config = {};
    chan_config.bitwidth = ADC_BITWIDTH_12;
    chan_config.atten = ADC_ATTEN_DB_12;
    adc_oneshot_config_channel(ulp_adc_handle, ADC_CHANNEL_0, &chan_config);
    adc_oneshot_config_channel(ulp_adc_handle, ADC_CHANNEL_1, &chan_config);

    // Define ULP program labels
    enum UlpLabel {
      L_ALREADY_SET = 1,
      L_CHECK_CH1,
      L_RIGHT,
      L_CHECK_CONFIRM,
      L_LEFT,
      L_CHECK_BACK,
      L_CONFIRM,
      L_BACK,
      L_UP,
      L_DOWN,
      L_WAKE,
      L_NONE_ALL
    };

    // Define ULP FSM program
    const ulp_insn_t ulp_prog[] = {
        // Check if a wakeup action is already pending (RTC_SLOW_MEM[0] >= 1)
        I_MOVI(R0, 0), I_LD(R1, R0, 0),  // Load RTC_SLOW_MEM[0] into R1
        I_MOVR(R0, R1),                  // Copy R1 to R0 for comparison
        M_BG(L_ALREADY_SET, 0),          // If R0 > 0 (>= 1), skip execution and halt

        // Initialize RTC_SLOW_MEM[0] (wakeup button ID) to 0 (None)
        I_MOVI(R0, 0), I_ST(R0, R0, 0),

        // Read ADC1 Channel 0 (GPIO1) into R1
        I_ADC(R1, 0, 0),

        // Read ADC1 Channel 1 (GPIO2) into R2
        I_ADC(R2, 0, 1),

        // --- Process Channel 0 ---
        I_MOVR(R0, R1), M_BL(L_RIGHT, 501),  // If < 501 (0..500): RIGHT button
        M_BL(L_CHECK_CONFIRM, 800),          // If < 800 (501..799): not LEFT, check CONFIRM
        M_BL(L_LEFT, 1501),                  // If < 1501 (800..1500): LEFT button

        M_LABEL(L_CHECK_CONFIRM), M_BL(L_CHECK_BACK, 2300),  // If < 2300 (1501..2299): not CONFIRM, check BACK
        M_BL(L_CONFIRM, 2701),                               // If < 2701 (2300..2700): CONFIRM button

        M_LABEL(L_CHECK_BACK), M_BL(L_CHECK_CH1, 3200),  // If < 3200 (2701..3199): not BACK, check Channel 1
        M_BL(L_BACK, 3601),                              // If < 3601 (3200..3600): BACK button
        M_BX(L_CHECK_CH1),                               // Otherwise: not BACK, check Channel 1

        // --- Actions for Channel 0 ---
        M_LABEL(L_RIGHT), I_MOVI(R3, 1),  // BTN_PAGE_FWD
        M_BX(L_WAKE),

        M_LABEL(L_LEFT), I_MOVI(R3, 2),  // BTN_PAGE_BACK
        M_BX(L_WAKE),

        M_LABEL(L_CONFIRM), I_MOVI(R3, 3),  // BTN_MENU
        M_BX(L_WAKE),

        M_LABEL(L_BACK), I_MOVI(R3, 4),  // BTN_HOME
        M_BX(L_WAKE),

        // --- Process Channel 1 ---
        M_LABEL(L_CHECK_CH1), I_MOVR(R0, R2), M_BL(L_UP, 501),  // If < 501 (0..500): UP button
        M_BL(L_NONE_ALL, 1900),                                 // If < 1900 (501..1899): not DOWN, skip
        M_BL(L_DOWN, 2301),                                     // If < 2301 (1900..2300): DOWN button
        M_BX(L_NONE_ALL),                                       // Otherwise: none, halt

        // --- Actions for Channel 1 ---
        M_LABEL(L_DOWN), I_MOVI(R3, 2),  // BTN_PAGE_BACK
        M_BX(L_WAKE),

        M_LABEL(L_UP), I_MOVI(R3, 1),  // BTN_PAGE_FWD
        M_BX(L_WAKE),

        // --- Finish / Wake ---
        M_LABEL(L_WAKE), I_MOVI(R0, 0), I_ST(R3, R0, 0),  // Store button ID in RTC_SLOW_MEM[0]
        I_WAKE(),                                         // Wake up main CPU
        I_HALT(),                                         // Stop ULP execution

        M_LABEL(L_ALREADY_SET),
        I_HALT(),  // Stop ULP execution if already pending

        M_LABEL(L_NONE_ALL),
        I_HALT()  // Stop ULP execution
    };

    // Load ULP program at offset 32 (reserving first 32 words for variables/alignment)
    constexpr uint32_t ULP_START_OFFSET = 32;
    size_t size = sizeof(ulp_prog) / sizeof(ulp_insn_t);
    ulp_process_macros_and_load(ULP_START_OFFSET, ulp_prog, &size);

    // Set ULP wakeup period to 100ms (100,000 us)
    ulp_set_wakeup_period(0, 100000);

    // Keep the RTC peripheral power domain enabled during sleep to preserve ADC configurations
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    // Enable ULP wakeup
    esp_sleep_enable_ulp_wakeup();

    // Start ULP
    ulp_run(ULP_START_OFFSET);
  }
#endif

  // Enter Deep Sleep
  // Always clear ULP wakeup register before sleeping
  // so the ULP guard check starts clean each sleep cycle
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  RtcState::sleepEntryTime = (uint32_t)tv.tv_sec;
  RtcState::pagesUntilFullRefreshRtc = pagesUntilFullRefresh;  // persist current cadence state
  RTC_SLOW_MEM[0] = 0;
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
