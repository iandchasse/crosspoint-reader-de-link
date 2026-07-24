#include "HalClock.h"

#include <Arduino.h>
#include <Logging.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_private/esp_clk.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <sys/time.h>
#include <time.h>
#include <Wire.h>

#include <cstdlib>
#include <cassert>

#include "HalGPIO.h"

HalClock halClock;  // Singleton instance

// ---- RTC-memory state (survives deep sleep, not cold boot) ----------------

static constexpr uint32_t CLOCK_RTC_MAGIC = 0xC10C4B1D;
static constexpr uint32_t CLOCK_RTC_FLAG_LP_VALID = 0x00000001u;

RTC_NOINIT_ATTR static uint32_t rtcClockMagic;
RTC_NOINIT_ATTR static uint32_t rtcClockFlags;
RTC_NOINIT_ATTR static time_t rtcEpoch;       // last-known unix epoch
RTC_NOINIT_ATTR static uint64_t rtcLpTimeUs;  // esp_clk_rtc_time() at capture
RTC_NOINIT_ATTR static uint32_t rtcSlowCal;   // esp_clk_slowclk_cal_get() at capture

static bool clockApproximate = true;

struct TimeZoneEntry {
  const char* tz;
};

static constexpr TimeZoneEntry TIMEZONES[] = {
    {"GMT0BST,M3.5.0/1,M10.5.0/2"},
    {"CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"MSK-3"},
    {"UTC-4"},
    {"UTC-5:30"},
    {"UTC-7"},
    {"UTC-8"},
    {"UTC-9"},
    {"AEST-10AEDT,M10.1.0/2,M4.1.0/3"},
    {"NZST-12NZDT,M9.5.0/2,M4.1.0/3"},
    {"UTC+3"},
    {"EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"PST8PDT,M3.2.0/2,M11.1.0/2"},
};

// ---- NVS helpers ----------------------------------------------------------

// If the last NTP sync is older than this, treat a cold-boot restore as
// unsynced rather than showing a potentially very wrong time.
static constexpr int64_t STALE_THRESHOLD_S = 72 * 3600;  // 72 hours

static constexpr char NVS_NAMESPACE[] = "halclock";
static constexpr char NVS_KEY[] = "epoch";
static constexpr char NVS_SYNC_KEY[] = "lastsync";

static void nvsWrite(time_t epoch) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putLong64(NVS_KEY, (int64_t)epoch);
    prefs.end();
  }
}

static void nvsWriteSyncTime(time_t syncEpoch) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putLong64(NVS_SYNC_KEY, (int64_t)syncEpoch);
    prefs.end();
  }
}

static time_t nvsRead() {
  Preferences prefs;
  time_t epoch = 0;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    epoch = (time_t)prefs.getLong64(NVS_KEY, 0);
    prefs.end();
  }
  return epoch;
}

static time_t nvsReadSyncTime() {
  Preferences prefs;
  time_t syncEpoch = 0;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    syncEpoch = (time_t)prefs.getLong64(NVS_SYNC_KEY, 0);
    prefs.end();
  }
  return syncEpoch;
}

// ---- internal helpers -----------------------------------------------------

static void setSystemClock(time_t epoch) {
  struct timeval tv = {};
  tv.tv_sec = epoch;
  settimeofday(&tv, nullptr);
}

static bool rtcValid() { return rtcClockMagic == CLOCK_RTC_MAGIC && rtcEpoch > 0; }

/// Capture current time + LP timer into RTC memory, and epoch into NVS.
static void capture(bool lpValid) {
  rtcEpoch = time(nullptr);
  rtcLpTimeUs = esp_clk_rtc_time();
  rtcSlowCal = esp_clk_slowclk_cal_get();
  rtcClockMagic = CLOCK_RTC_MAGIC;
  rtcClockFlags = lpValid ? CLOCK_RTC_FLAG_LP_VALID : 0;
  nvsWrite(rtcEpoch);
}

// ---- public API (Static facade methods for native clock) ------------------

void HalClock::applyTimezone(uint8_t timeZoneSetting) {
  const size_t index = timeZoneSetting < (sizeof(TIMEZONES) / sizeof(TIMEZONES[0])) ? timeZoneSetting : 0;
  setenv("TZ", TIMEZONES[index].tz, 1);
  tzset();
  LOG_DBG("CLK", "Timezone applied: %s", TIMEZONES[index].tz);
}

bool HalClock::syncNtp() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }

  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  int retry = 0;
  constexpr int maxRetries = 50;  // 5 seconds
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < maxRetries) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    retry++;
  }

  if (retry >= maxRetries) {
    LOG_ERR("CLK", "NTP sync timeout");
    return false;
  }

  capture(false);
  nvsWriteSyncTime(rtcEpoch);
  clockApproximate = false;
  LOG_INF("CLK", "NTP synced, epoch %lld", (long long)rtcEpoch);
  return true;
}

void HalClock::saveBeforeSleep(bool keepLpAlive) {
  if (!isSynced()) {
    return;
  }
  capture(keepLpAlive);
  LOG_DBG("CLK", "Saved epoch %lld before sleep", (long long)rtcEpoch);
}

void HalClock::restore() {
  const bool lpValid = (rtcClockFlags & CLOCK_RTC_FLAG_LP_VALID) != 0;
  if (rtcValid() && lpValid) {
    // RTC memory survived — we woke from deep sleep.
    // The ESP32-S3 natively preserves `gettimeofday()` across deep sleep using the RTC timer.
    // The existing TimeUtil::correctTimeOnWake() will apply exact calibration for the internal RC oscillator drift.
    // Therefore, we do NOT attempt to manually calculate elapsed time here (which was causing a 2-hour drift).
    clockApproximate = true;
    LOG_INF("CLK", "Restored from Native ESP-IDF RTC (TimeUtil will calibrate)");
    return;
  }

  // Item C: software restart (ESP.restart / silentRestart for post-Wi-Fi heap
  // reclaim, OTA, etc.). Unlike a cold boot, the RTC timer keeps running across
  // esp_restart() and the IDF preserves gettimeofday() — the same mechanism the
  // deep-sleep branch above relies on. The lpValid flag isn't set for a reboot,
  // so without this branch a silentRestart falls to the NVS path below and loses
  // all wall-clock time since the last NVS write (potentially hours). Trust native
  // time when RTC memory survived (rtcValid) and the clock is valid — we lose only
  // the ~1-2 s of the reboot itself.
  if (esp_reset_reason() == ESP_RST_SW && rtcValid() && isSynced()) {
    clockApproximate = true;
    LOG_INF("CLK", "Restored from native RTC across software restart (epoch %lld)", (long long)time(nullptr));
    return;
  }

  // Cold boot — try NVS.  No elapsed correction possible.
  time_t epoch = nvsRead();
  if (epoch > 0) {
    time_t lastSync = nvsReadSyncTime();
    if (lastSync > 0 && (epoch - lastSync) > STALE_THRESHOLD_S) {
      LOG_ERR("CLK", "NVS epoch %lld is stale (last NTP sync %lld, %lld h ago), discarding", (long long)epoch,
              (long long)lastSync, (long long)((epoch - lastSync) / 3600));
      return;
    }
    setSystemClock(epoch);
    rtcEpoch = epoch;
    rtcLpTimeUs = esp_clk_rtc_time();
    rtcSlowCal = esp_clk_slowclk_cal_get();
    rtcClockMagic = CLOCK_RTC_MAGIC;
    rtcClockFlags = 0;
    clockApproximate = true;
    LOG_INF("CLK", "Restored from NVS, epoch %lld (no elapsed correction)", (long long)epoch);
  }
}  

time_t HalClock::now() {
  if (!isSynced()) {
    return 0;
  }
  return time(nullptr);
}

bool HalClock::isSynced() {
  return time(nullptr) > 1577836800;  // > 2020-01-01
}

bool HalClock::isApproximate() { return clockApproximate; }

void HalClock::formatTime(char* buf, size_t bufSize, bool use24h) {
  if (!isSynced()) {
    snprintf(buf, bufSize, "--:--");
    return;
  }

  time_t t = time(nullptr);
  struct tm timeinfo;
  localtime_r(&t, &timeinfo);

  if (use24h) {
    snprintf(buf, bufSize, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  } else {
    int hour = timeinfo.tm_hour % 12;
    if (hour == 0) hour = 12;
    const char* ampm = timeinfo.tm_hour < 12 ? "AM" : "PM";
    snprintf(buf, bufSize, "%d:%02d%s", hour, timeinfo.tm_min, ampm);
  }
}

void HalClock::formatLogTime(char* buf, size_t bufSize) {
  if (!isSynced()) {
    buf[0] = '\0';
    return;
  }

  time_t t = time(nullptr);
  struct tm timeinfo;
  localtime_r(&t, &timeinfo);
  snprintf(buf, bufSize, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

void HalClock::wifiOff(bool skipNtpSync) {
  if (!skipNtpSync && isApproximate() && WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED) {
    syncNtp();
  }
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}


// ---- DS3231 register layout (BCD encoded): --------------------------------
//   0x00: Seconds  (bits 6-4 = tens, bits 3-0 = ones)
//   0x01: Minutes  (bits 6-4 = tens, bits 3-0 = ones)
//   0x02: Hours    (bit 6 = 12/24 mode, bits 5-4 = tens, bits 3-0 = ones)

static constexpr uint8_t I2C_ADDR_DS3231 = 0x68;
static constexpr uint8_t DS3231_SEC_REG = 0x00;

static uint8_t bcdToDec(uint8_t bcd) { return ((bcd >> 4) * 10) + (bcd & 0x0F); }
static uint8_t decToBcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

void HalClock::begin() {
  if (!gpio.deviceIsX3()) {
    _available = false;
    return;
  }

  // I2C is already initialised by HalPowerManager::begin() for X3.
  // Probe the DS3231 by reading the seconds register.
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    LOG_INF("CLK", "DS3231 RTC not found");
    _available = false;
    return;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)1);
  if (Wire.available() < 1) {
    _available = false;
    return;
  }
  Wire.read();  // discard — just testing connectivity

  _available = true;
  LOG_INF("CLK", "DS3231 RTC found");

  // Prime the cache with an initial read
  uint8_t h, m;
  getTime(h, m);
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  // Read 3 bytes starting at register 0x00: seconds, minutes, hours
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)3);
  if (Wire.available() < 3) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Wire.read();  // seconds — not needed
  const uint8_t rawMin = Wire.read();
  const uint8_t rawHour = Wire.read();

  _cachedMinute = bcdToDec(rawMin & 0x7F);
  // Handle 12/24h mode: bit 6 high = 12h mode
  if (rawHour & 0x40) {
    // 12h mode: bit 5 = PM, bits 4-0 = hours (1-12)
    uint8_t h12 = bcdToDec(rawHour & 0x1F);
    bool pm = rawHour & 0x20;
    if (h12 == 12) h12 = 0;
    _cachedHour = pm ? (h12 + 12) : h12;
  } else {
    // 24h mode: bits 5-0 = hours (0-23)
    _cachedHour = bcdToDec(rawHour & 0x3F);
  }
  _lastPollMs = now;
  _hasCachedTime = true;

  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second) {
  assert(hour < 24);
  assert(minute < 60);
  assert(second < 60);
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);    // Start at register 0x00
  Wire.write(decToBcd(second));  // 0x00: Seconds
  Wire.write(decToBcd(minute));  // 0x01: Minutes
  Wire.write(decToBcd(hour));    // 0x02: Hours (24h mode, bit 6 = 0)
  if (Wire.endTransmission() != 0) {
    LOG_ERR("CLK", "Failed to write time to DS3231");
    return false;
  }

  // Invalidate cache so next read fetches fresh data
  _lastPollMs = 0;
  _cachedHour = hour;
  _cachedMinute = minute;
  _hasCachedTime = true;
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      if (writeTimeToRTC(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec)) {
        LOG_INF("CLK", "RTC set to %02d:%02d:%02d UTC", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
