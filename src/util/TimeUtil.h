#pragma once

#include <Arduino.h>

#include <cstdint>
#include <string>


class TimeUtil {
 public:
  /**
   * Reconfigures the system timezone based on current settings.
   * Delegates to HalClock::applyTimezone() for POSIX TZ handling.
   */
  static void reconfigure();

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

  /**
   * Opportunistic NTP sync + calibration in one call: runs HalClock::syncNtp()
   * and, on success, feeds onNtpSynced(). Every place that has Wi-Fi up should
   * route through this so no sync opportunity is wasted on the drift learner.
   * Returns true if the sync succeeded. Cheap no-op cost if already synced.
   */
  static bool syncAndCalibrate();

  /**
   * TEMP(diag): log one passive (period_us, tempC) sample to rtc_diag.jsonl for a
   * mid-sleep timer wake. Does NOT touch the drift bracket or install a calibration.
   * Gated at the call site by -DRTC_DIAG_TICK_S; remove with the rest of temp(diag).
   */
  static void logDiagTick();

 private:
  struct CalibrationData {
    int64_t sleepStartTime = 0;      // time_t when we entered deep sleep
    int64_t lastNtpTime = 0;         // time_t of last successful NTP sync
    double driftRatio = 1.0;         // RTC ticks / real ticks (>1 = RTC runs fast)
    int64_t driftRatioUpdatedAt = 0; // time_t driftRatio was last (re)measured; 0 = never
  };

  static bool loadCalibration(CalibrationData& data);
  static bool saveCalibration(const CalibrationData& data);
};
