#pragma once

#include <Arduino.h>

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
};
