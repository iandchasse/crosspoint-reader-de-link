#pragma once
#include <stdint.h>

// RTC-persisted state surviving deep sleep.
// Defined in HalPowerManager.cpp.
namespace RtcState {
extern uint32_t sleepEntryTime;
extern uint8_t pagesUntilFullRefreshRtc;
}  // namespace RtcState