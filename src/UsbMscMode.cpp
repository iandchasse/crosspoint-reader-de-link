#include "UsbMscMode.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FreeInkUsbMsc.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <USB.h>
#include <USBMSC.h>
#include <tusb.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "fontIds.h"

// External globals from main.cpp
extern HalDisplay display;
extern HalGPIO gpio;
extern GfxRenderer renderer;
extern FontDecompressor fontDecompressor;
extern FontCacheManager fontCacheManager;
extern EpdFontFamily ui12FontFamily;

namespace {

// SD sector access lives in the SDK; the USBMSC LUN that forwards to it lives
// here, because the Arduino USB stack is the application's to own -- the SDK
// stays independent of which USB mode a consumer's build selects.
freeink::UsbMsc usbMsc;
USBMSC msc;

bool startUsbMsc() {
  if (!usbMsc.begin()) return false;

  msc.vendorID("ESP32S3");
  msc.productID("EPDReader");
  msc.productRevision("1.0");
  msc.onRead([](uint32_t lba, uint32_t, void* buffer, uint32_t bufsize) -> int32_t {
    return usbMsc.readSectors(lba, buffer, bufsize);
  });
  msc.onWrite([](uint32_t lba, uint32_t, uint8_t* buffer, uint32_t bufsize) -> int32_t {
    return usbMsc.writeSectors(lba, buffer, bufsize);
  });
  msc.onStartStop([](uint8_t, bool, bool) -> bool {
    // The host ejecting the drive is its last chance to have data committed.
    usbMsc.flush();
    return true;
  });
  msc.mediaPresent(true);
  msc.begin(usbMsc.sectorCount(), usbMsc.sectorSize());

  // Settle before handing the USB peripheral to TinyUSB.
  //
  // USB.begin() allocates the USB interrupt via esp_intr_alloc(), which runs on
  // the other core through the IPC task. Racing that against work still landing
  // from the preceding init -- notably the priority-5 "usbmsc_write" task that
  // freeink::UsbMsc::begin() has just spawned, plus SDMMC and the e-ink refresh --
  // faults inside the heap allocator on the IPC task, with a corrupted backtrace
  // and ~215KB free (so it is not exhaustion).
  //
  // This is a mitigation, not a root-cause fix: the underlying race is in the
  // Arduino USB stack's interrupt setup, not in this file. The SDK already uses
  // the same idiom (UsbMsc::end() does forceSerialJtagPhy(); delay(100)). Without
  // it the crash is reproducible on nearly every entry; with it, MSC comes up
  // reliably. Do not remove without re-testing MSC entry ~10 times.
  delay(100);

  USB.begin();
  return true;
}

// Hold duration to leave the mode. Long enough that it can't be hit by accident
// while the device is sitting plugged into a host.
constexpr unsigned long EXIT_HOLD_MS = 5000;

void drawStatus(const char* line, const int height, const bool showExitHint) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, height / 2 - 40, "USB Mass Storage Mode", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, height / 2, line, true, EpdFontFamily::REGULAR);
  if (showExitHint) {
    renderer.drawCenteredText(UI_12_FONT_ID, height / 2 + 40, "Hold Power Button for 5s to exit", true,
                              EpdFontFamily::REGULAR);
  }
}

// Leaving MSC mode is a reboot: the host has owned the FAT while we were
// attached, so nothing in this firmware's caches can be trusted afterwards.
void restartToNormalMode() {
  // Soft-disconnect first so the host unmounts cleanly rather than reporting a
  // surprise removal; the SDK then flushes and returns the PHY to serial/JTAG.
  if (tud_inited()) {
    tud_disconnect();
    delay(100);
  }
  usbMsc.end();
  ESP.restart();
}

}  // namespace

void forceUsbSerialJtag() { freeink::UsbMsc::forceSerialJtagPhy(); }

void runUsbMscMode() {
  usbMscBootFlag = 0;

  gpio.begin();
  display.begin(false);
  renderer.begin();

  if (!fontDecompressor.init()) {
    // Non-fatal: text still renders from the built-in font.
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);

  const auto height = renderer.getScreenHeight();

  drawStatus("Connecting as USB Drive...", height, true);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);

  if (!Storage.begin()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, height / 2, "SD Card Init Failed!", true, EpdFontFamily::BOLD);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    delay(5000);
    restartToNormalMode();
  }

  if (!startUsbMsc()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, height / 2, "USB Storage Failed!", true, EpdFontFamily::BOLD);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    delay(5000);
    restartToNormalMode();
  }

  drawStatus("Connected as USB Drive", height, true);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);

  unsigned long powerHeldSince = 0;
  while (true) {
    gpio.update();
    if (gpio.isPressed(HalGPIO::BTN_POWER)) {
      if (powerHeldSince == 0) {
        powerHeldSince = millis();
      } else if (millis() - powerHeldSince >= EXIT_HOLD_MS) {
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, height / 2, "Restarting...", true, EpdFontFamily::BOLD);
        renderer.displayBuffer(HalDisplay::FULL_REFRESH);
        delay(1000);
        restartToNormalMode();
      }
    } else {
      powerHeldSince = 0;
    }
    delay(50);
  }
}
