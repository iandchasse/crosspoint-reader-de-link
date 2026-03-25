#include "TimeUtil.h"

#include <Arduino.h>
#include <ArduinoJson.h>
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

// Raw RTC time captured on wake before correction is applied.
// Used by onNtpSynced() to compute actual drift.
int64_t rtcWakeTime = 0;
}  // namespace

void TimeUtil::reconfigure() {
  const int gmtOffset_sec = (static_cast<int>(SETTINGS.timezoneOffsetHours) - 12) * 3600;
  const int daylightOffset_sec = 0;

  LOG_INF("TIME", "Reconfiguring time with offset %d seconds (UTC%+d)", gmtOffset_sec, (gmtOffset_sec / 3600));

  // We call configTime regardless of WiFi status to update the system timezone offset.
  // The SNTP client will automatically attempt to sync once WiFi is connected.
  configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "time.nist.gov", "time.google.com");
}

bool TimeUtil::isTimeValid() {
  time_t now;
  ::time(&now);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  // Consider time valid if it's after Jan 1st 2024
  return timeinfo.tm_year >= (2024 - 1900);
}

bool TimeUtil::isSynced() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
  return sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
#else
  return sntp_get_sync_status() != SNTP_SYNC_STATUS_RESET;
#endif
}

std::string TimeUtil::getFormattedTime() {
  if (!isTimeValid()) {
    return "";
  }

  time_t now;
  ::time(&now);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  char timeStr[16];
  if (SETTINGS.use24HourClock) {
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  } else {
    int hour = timeinfo.tm_hour % 12;
    if (hour == 0) hour = 12;
    snprintf(timeStr, sizeof(timeStr), "%d:%02d %s", hour, timeinfo.tm_min, (timeinfo.tm_hour >= 12) ? "PM" : "AM");
  }
  return std::string(timeStr);
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
  return true;
}

bool TimeUtil::saveCalibration(const CalibrationData& data) {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["sleepStartTime"] = data.sleepStartTime;
  doc["lastNtpTime"] = data.lastNtpTime;
  doc["driftRatio"] = data.driftRatio;

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(RTC_CAL_FILE, json);
}

void TimeUtil::recordSleepEntry() {
  time_t now;
  ::time(&now);

  if (!isTimeValid()) {
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
  if (!isTimeValid()) {
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
  saveCalibration(data);

  // Reset for next cycle
  rtcWakeTime = 0;
}
