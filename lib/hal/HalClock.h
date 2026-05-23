#pragma once

#include <cstdint>
#include <ctime>

// Unified HalClock class supporting both native clock (ESP32-S3 LP-timer & NVS)
// and external DS3231 RTC (for X3 compatibility).
class HalClock {
  bool _available = false;
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // --- Static facade methods for native clock (S3-port) ---
  
  /// Perform an NTP sync (requires WiFi to be connected). Starts SNTP,
  /// waits up to 5 seconds for completion, then captures the result.
  /// Returns true if the sync succeeded.
  static bool syncNtp();

  /// Apply timezone/DST rules via the POSIX TZ string for the given setting.
  static void applyTimezone(uint8_t timeZoneSetting);

  /// Call just before deep sleep. Snapshots the current system time to RTC
  /// memory and NVS so it can be restored on wake / cold boot. Pass true when
  /// the LP timer is kept alive during sleep.
  static void saveBeforeSleep(bool keepLpAlive);

  /// Call on boot to seed the system clock from the best available stored
  /// value. When RTC memory is valid (deep-sleep wake) and the LP timer was
  /// running, the restored time includes elapsed-time correction. Falls back
  /// to NVS for cold boot.
  static void restore();

  /// Returns the current best-effort wall-clock epoch, or 0 if the clock was
  /// never set.
  static time_t now();

  /// True if the clock has been set at least once (NTP or restore).
  static bool isSynced();

  /// True if the last restore was from a backup (not NTP) — i.e. the clock
  /// may have drifted. Cleared on NTP sync.
  static bool isApproximate();

  /// Format the current time for display. Returns "--:--" if the clock was
  /// never synced, prefixes with "~" if approximate.
  /// When use24h is false, formats as "2:05pm" / "12:30am".
  /// Output is written to `buf` (must be at least 16 bytes).
  static void formatTime(char* buf, size_t bufSize, bool use24h);

  /// Format the current time for log timestamps. Returns "HH:MM:SS" if
  /// synced, or an empty string if not.
  static void formatLogTime(char* buf, size_t bufSize);

  /// Tear down WiFi cleanly. When skipNtpSync is false (default) and the
  /// clock is approximate, performs an opportunistic NTP sync before
  /// disconnecting — essentially free since we already have a connection.
  static void wifiOff(bool skipNtpSync = false);


  // --- Instance methods for DS3231 RTC (X3 compatibility) ---

  // Call after gpio.begin() and powerManager.begin() (I2C already initialised for X3)
  void begin();

  // True if the DS3231 RTC is present on this device
  bool isAvailable() const { return _available; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Sync the DS3231 RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  bool syncFromNTP();

 private:
  bool writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second);
};

extern HalClock halClock;  // Singleton
