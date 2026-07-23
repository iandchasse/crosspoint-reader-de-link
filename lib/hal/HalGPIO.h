#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>

// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#define EPD_SCLK 10  // SPI Clock
#define EPD_MOSI 9   // SPI MOSI (Master Out Slave In)
#define EPD_CS 11    // Chip Select
#define EPD_DC 12    // Data/Command
#define EPD_RST 13   // Reset
#define EPD_BUSY 14  // Busy

#define SPI_MISO 7  // SPI MISO, shared between SD card and display (Master In Slave Out)

#define BAT_GPIO4 4  // Battery voltage
#define BAT_CHECK 8  // STAT charging pin

#define UART0_RXD 20  // Used for USB connection detection

class HalGPIO {
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
#endif

  bool lastUsbConnected = false;
  bool usbStateChanged = false;

 public:
  HalGPIO() = default;

  // Inline device type helpers (this platform is always X4/S3)
  inline bool deviceIsX3() const { return false; }
  inline bool deviceIsX4() const { return true; }

  // Start button GPIO and setup SPI for screen and SD card
  void begin();

  // Button input methods
  void update();
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  unsigned long getPowerButtonHeldTime() const;

  // Verify power button was held long enough after wakeup.
  // Returns true if verification succeeded, false if device should return to sleep.
  // Should only be called when wakeup reason is PowerButton.
  bool verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed);

  // Check if USB is connected
  bool isUsbConnected() const;

  // Check if battery is actively charging (MCP73832 STAT pin, GPIO8)
  bool isCharging() const;

  // Returns true once per edge (plug or unplug) since the last update()
  bool wasUsbStateChanged() const;

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };

  WakeupReason getWakeupReason() const;

  // Button indices. These mirror InputManager's, so they must stay in lockstep
  // with the SDK -- static_assert below enforces it rather than trusting comments.
  static constexpr uint8_t BTN_BACK = InputManager::BTN_BACK;
  static constexpr uint8_t BTN_CONFIRM = InputManager::BTN_CONFIRM;
  static constexpr uint8_t BTN_LEFT = InputManager::BTN_LEFT;
  static constexpr uint8_t BTN_RIGHT = InputManager::BTN_RIGHT;
  static constexpr uint8_t BTN_UP = InputManager::BTN_UP;
  static constexpr uint8_t BTN_DOWN = InputManager::BTN_DOWN;
  static constexpr uint8_t BTN_POWER = InputManager::BTN_POWER;
  // Second ADC combo group — physical duplicates of BTN_UP/BTN_DOWN on this board.
  static constexpr uint8_t BTN_UP_2 = InputManager::BTN_UP_2;
  static constexpr uint8_t BTN_DOWN_2 = InputManager::BTN_DOWN_2;
};

// Only the four front buttons are persisted (CrossPointSettings::frontButton*),
// so those four indices are the ABI: changing them would silently remap a user's
// saved button assignments. The second side pair sits after BTN_POWER for the
// same reason.
static_assert(HalGPIO::BTN_BACK == 0 && HalGPIO::BTN_CONFIRM == 1 && HalGPIO::BTN_LEFT == 2 &&
                  HalGPIO::BTN_RIGHT == 3,
              "Persisted front-button indices must not change");

extern HalGPIO gpio;  // Singleton
