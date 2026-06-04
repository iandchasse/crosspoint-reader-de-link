#pragma once

#include <esp_attr.h>
#include <cstdint>

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();          // home screen
void silentRestartToReader();  // currently-open EPUB (APP_STATE.openEpubPath)

extern uint32_t usbMscBootFlag;
constexpr uint32_t USB_MSC_BOOT_MAGIC = 0xDEADC0DE;


