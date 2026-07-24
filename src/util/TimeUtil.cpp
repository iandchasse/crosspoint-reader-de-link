#include "TimeUtil.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include "CrossPointSettings.h"
#include "Logging.h"

namespace {
constexpr char RTC_CAL_FILE[] = "/.crosspoint/rtc_cal.json";
constexpr double SMOOTHING_ALPHA = 0.3;       // EMA smoothing factor
constexpr int64_t MIN_ELAPSED_SECONDS = 60;    // Minimum sleep to calibrate from

// A3 fail-safe: the drift ratio is a single global scalar with no temperature
// term, so a ratio learned days ago (likely at a different temperature) can
// correct with the wrong magnitude or sign. Past this age, applying it does more
// harm than good — skip correction and fall back to the raw native RTC, which is
// at least self-consistent. A regularly-synced device re-stamps this well within
// the window; only long offline stretches trip it.
constexpr int64_t MAX_CALIBRATION_AGE_SECONDS = 48 * 3600;  // 48 h

// Raw RTC time captured on wake before correction is applied.
// Used by onNtpSynced() to compute actual drift.
int64_t rtcWakeTime = 0;
}  // namespace

void TimeUtil::reconfigure() {
  HalClock::applyTimezone(SETTINGS.timeZone);
  LOG_INF("TIME", "Reconfigured timezone to index %d", SETTINGS.timeZone);
}

// ---- RTC Drift Calibration ----

bool TimeUtil::loadCalibration(CalibrationData& data) {
  if (!Storage.exists(RTC_CAL_FILE)) {
    return false;
  }

  String json = Storage.readFile(RTC_CAL_FILE);
  if (json.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("RTC_CAL", "JSON parse error: %s", error.c_str());
    return false;
  }

  data.sleepStartTime = doc["sleepStartTime"] | (int64_t)0;
  data.lastNtpTime = doc["lastNtpTime"] | (int64_t)0;
  data.driftRatio = doc["driftRatio"] | 1.0;
  // Absent in pre-A3 files: treat as "unknown age". Fall back to lastNtpTime so an
  // existing ratio isn't instantly considered stale on the first boot after upgrade.
  data.driftRatioUpdatedAt = doc["driftRatioUpdatedAt"] | data.lastNtpTime;
  return true;
}

bool TimeUtil::saveCalibration(const CalibrationData& data) {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["sleepStartTime"] = data.sleepStartTime;
  doc["lastNtpTime"] = data.lastNtpTime;
  doc["driftRatio"] = data.driftRatio;
  doc["driftRatioUpdatedAt"] = data.driftRatioUpdatedAt;

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(RTC_CAL_FILE, json);
}

void TimeUtil::recordSleepEntry() {
  time_t now;
  ::time(&now);

  if (!HalClock::isSynced()) {
    LOG_DBG("RTC_CAL", "Time not valid, skipping sleep entry record");
    return;
  }

  CalibrationData data;
  loadCalibration(data);  // Load existing data to preserve driftRatio and lastNtpTime
  data.sleepStartTime = static_cast<int64_t>(now);

  if (saveCalibration(data)) {
    LOG_DBG("RTC_CAL", "Recorded sleep entry at %lld", data.sleepStartTime);
  } else {
    LOG_ERR("RTC_CAL", "Failed to save sleep entry");
  }
}

void TimeUtil::correctTimeOnWake() {
  CalibrationData data;
  if (!loadCalibration(data)) {
    LOG_DBG("RTC_CAL", "No calibration data found, skipping correction");
    return;
  }

  if (data.sleepStartTime == 0) {
    LOG_DBG("RTC_CAL", "No sleep start time, skipping correction");
    return;
  }

  time_t now;
  ::time(&now);
  const int64_t rtcNow = static_cast<int64_t>(now);
  const int64_t rtcElapsed = rtcNow - data.sleepStartTime;

  // Always capture the raw RTC wake time for onNtpSynced() to use later
  rtcWakeTime = rtcNow;

  if (data.driftRatio == 1.0) {
    LOG_DBG("RTC_CAL", "No drift data yet, skipping correction (captured rtcWakeTime=%lld)", rtcWakeTime);
    return;
  }

  // A3 fail-safe: refuse to apply a stale (or malformed) drift ratio. rtcNow is
  // the uncorrected RTC time, which is more than accurate enough to judge an age
  // measured in days.
  const int64_t calibrationAge = rtcNow - data.driftRatioUpdatedAt;
  if (data.driftRatioUpdatedAt <= 0 || calibrationAge > MAX_CALIBRATION_AGE_SECONDS) {
    LOG_INF("RTC_CAL", "Drift ratio stale (age %lldh, ratio=%.6f); skipping correction, using raw RTC",
            (long long)(calibrationAge / 3600), data.driftRatio);
    return;
  }

  if (rtcElapsed < MIN_ELAPSED_SECONDS) {
    LOG_DBG("RTC_CAL", "Sleep too short (%llds), skipping correction", rtcElapsed);
    return;
  }

  // Correct: if RTC runs fast (ratio > 1), real elapsed time is less than RTC elapsed
  const int64_t correctedElapsed = static_cast<int64_t>(static_cast<double>(rtcElapsed) / data.driftRatio);
  const int64_t correctedTime = data.sleepStartTime + correctedElapsed;
  const int64_t correction = correctedElapsed - rtcElapsed;

  struct timeval tv;
  tv.tv_sec = static_cast<time_t>(correctedTime);
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);

  LOG_INF("RTC_CAL", "Corrected %llds of drift (slept %llds, ratio=%.6f)",
          correction, rtcElapsed, data.driftRatio);
}

void TimeUtil::onNtpSynced() {
  if (!HalClock::isSynced()) {
    LOG_DBG("RTC_CAL", "Time not valid after NTP sync, skipping calibration");
    return;
  }

  time_t now;
  ::time(&now);
  const int64_t actualNow = static_cast<int64_t>(now);

  CalibrationData data;
  loadCalibration(data);  // OK if file doesn't exist yet — defaults are fine

  if (data.lastNtpTime == 0 || data.sleepStartTime == 0) {
    // First sync ever, or no sleep data — just record baseline
    data.lastNtpTime = actualNow;
    saveCalibration(data);
    LOG_INF("RTC_CAL", "NTP synced, recorded baseline at %lld", actualNow);
    return;
  }

  if (rtcWakeTime == 0) {
    // correctTimeOnWake didn't run (e.g. first boot or no sleep occurred)
    data.lastNtpTime = actualNow;
    saveCalibration(data);
    LOG_INF("RTC_CAL", "No pre-correction RTC time available, updating baseline");
    return;
  }

  // Three known timestamps:
  //   sleepStartTime: NTP-accurate time when we entered sleep
  //   rtcWakeTime:    raw RTC time when we woke (before any correction)
  //   actualNow:      NTP-accurate time right now
  const int64_t actualSleepDuration = actualNow - data.sleepStartTime;
  const int64_t rtcSleepDuration = rtcWakeTime - data.sleepStartTime;

  if (rtcSleepDuration < MIN_ELAPSED_SECONDS || actualSleepDuration < MIN_ELAPSED_SECONDS) {
    data.lastNtpTime = actualNow;
    saveCalibration(data);
    LOG_DBG("RTC_CAL", "Sleep too short (rtc=%llds, actual=%llds), updating baseline",
            rtcSleepDuration, actualSleepDuration);
    rtcWakeTime = 0;
    return;
  }

  // Drift ratio for this sleep cycle: how many RTC seconds per real second
  const double cycleRatio = static_cast<double>(rtcSleepDuration) / static_cast<double>(actualSleepDuration);

  // Smooth with exponential moving average
  const double newRatio = SMOOTHING_ALPHA * cycleRatio + (1.0 - SMOOTHING_ALPHA) * data.driftRatio;

  LOG_INF("RTC_CAL", "Cycle drift: %.6f, smoothed: %.6f -> %.6f (slept %llds)",
          cycleRatio, data.driftRatio, newRatio, actualSleepDuration);

  data.driftRatio = newRatio;
  data.lastNtpTime = actualNow;
  data.driftRatioUpdatedAt = actualNow;  // A3: stamp freshness so wake-time correction can trust it
  saveCalibration(data);

  // Reset for next cycle
  rtcWakeTime = 0;
}
