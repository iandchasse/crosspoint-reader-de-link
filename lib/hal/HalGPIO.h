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

  // Check if USB is connected (uses native ESP-IDF USB Serial/JTAG API)
  bool isUsbConnected() const;

  // Check if battery is actively charging (MCP73832 STAT pin, GPIO8)
  bool isCharging() const;

  // Returns true once per edge (plug or unplug) since the last update()
  bool wasUsbStateChanged() const;

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };

  WakeupReason getWakeupReason() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  // Second ADC combo group — physical duplicates of BTN_UP/BTN_DOWN on this board.
  static constexpr uint8_t BTN_UP_2 = 6;    // Same logical action as BTN_UP
  static constexpr uint8_t BTN_DOWN_2 = 7;  // Same logical action as BTN_DOWN
  static constexpr uint8_t BTN_POWER = 8;   // Matches InputManager::BTN_POWER
};

extern HalGPIO gpio;  // Singleton
