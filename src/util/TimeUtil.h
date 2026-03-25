#pragma once

#include <Arduino.h>

#include <cstdint>
#include <string>


class TimeUtil {
 public:
  /**
   * Reconfigures the system time and timezone based on current settings.
   * If WiFi is connected, it also triggers an NTP sync.
   * This should be called whenever the timezone setting or WiFi status changes.
   */
  static void reconfigure();

  /**
   * Returns true if the system time has been successfully synced with NTP
   * or has been set to a valid value (e.g. from a past sync).
   */
  static bool isTimeValid();

  /**
   * Returns true if SNTP is currently in a "completed" sync state.
   */
  static bool isSynced();

  /**
   * Get a formatted time string (e.g. "12:34 PM" or "12:34") based on user settings.
   */
  static std::string getFormattedTime();

  /**
   * Record the current time before entering deep sleep.
   * This snapshot is used on the next wake to calculate RTC drift.
   */
  static void recordSleepEntry();

  /**
   * Apply drift correction immediately after waking from deep sleep.
   * Uses the stored drift ratio to adjust the RTC time before NTP is available.
   * Should be called early in setup(), after SD card and TimeUtil::reconfigure().
   */
  static void correctTimeOnWake();

  /**
   * Called after a successful NTP sync to measure and record RTC drift.
   * Compares the RTC-elapsed time against the actual NTP-elapsed time since the
   * last sync to update the smoothed drift ratio stored on the SD card.
   */
  static void onNtpSynced();

 private:
  struct CalibrationData {
    int64_t sleepStartTime = 0;  // time_t when we entered deep sleep
    int64_t lastNtpTime = 0;     // time_t of last successful NTP sync
    double driftRatio = 1.0;     // RTC ticks / real ticks (>1 = RTC runs fast)
  };

  static bool loadCalibration(CalibrationData& data);
  static bool saveCalibration(const CalibrationData& data);
};
