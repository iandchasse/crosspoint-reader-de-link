#include "TimeUtil.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include "CrossPointSettings.h"
#include "Logging.h"

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
