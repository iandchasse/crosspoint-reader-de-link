#include "TimeUtil.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <WiFi.h>
#include <esp_private/esp_clk.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <soc/rtc.h>
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

// Item E: reject implausible drift samples. A single global drift ratio should sit
// very close to 1.0 — the boot calibration already removes the RC's nominal offset,
// leaving only temperature drift (a few % at the extreme). Anything beyond ±5% is
// not real RC drift but a bad sample: an NTP reply with a large/asymmetric round-
// trip delay skewing the reference, or residual awake-time contamination. Learning
// from it would drag the EMA toward garbage, so we drop the sample instead.
constexpr double MIN_PLAUSIBLE_RATIO = 0.95;
constexpr double MAX_PLAUSIBLE_RATIO = 1.05;

// Raw RTC time captured on wake before correction is applied.
// Used by onNtpSynced() to compute actual drift.
int64_t rtcWakeTime = 0;

// Item D: monotonic (crystal-based) microseconds captured at the same instant as
// rtcWakeTime. onNtpSynced() subtracts the awake interval since wake so the drift
// sample reflects ONLY the sleep, not the reading time before the user connected
// Wi-Fi. Without this, actualSleepDuration includes awake time and the learned
// ratio is biased low (and the EMA converges to that bias, not the truth).
int64_t wakeMonotonicUs = 0;

// Per-sleep bracket state in RTC_NOINIT, NOT on the SD card. An SD write issued
// milliseconds before esp_deep_sleep_start() is not reliably flushed to the card,
// so rtc_cal.json's sleepStartTime could not be read back on wake -> correctTime-
// OnWake() bailed at loadCalibration() -> rtcWakeTime never set -> the drift learner
// was permanently starved (every sync logged "No pre-correction"). RTC_NOINIT is
// designed to survive a deep-sleep wake, so the bracket start lives here. (SD keeps
// the long-term driftRatio/lastNtpTime, which must survive a full power-off.)
RTC_NOINIT_ATTR int64_t rtcSleepStartEpoch;
RTC_NOINIT_ATTR uint32_t rtcSleepStartMagic;
constexpr uint32_t SLEEP_START_MAGIC = 0x5CA1B5A7;

// The sleep-start resolved for the current wake's bracket (0 = none). Set by
// correctTimeOnWake() from RTC_NOINIT, consumed by onNtpSynced().
int64_t bracketSleepStart = 0;

// TEMP DIAGNOSTIC: correctTimeOnWake runs ~355 ms into a deep-sleep-wake boot,
// before USB-CDC serial re-enumerates (~300 ms), so its logs are lost. Stash what
// it saw here and re-print at the next sync, which is always well past reconnect.
bool dbgWakeRan = false;
bool dbgWakeRtcValid = false;
int64_t dbgWakeSleepStart = 0;
int dbgWakeResetReason = 0;

// A2: cycles to measure the RTC slow clock (~136 kHz internal RC) period at
// sleep-entry. The value of doing this here is the TIMING — the period is captured
// right before sleep at the current temperature, rather than only at boot/wake.
// The cycle COUNT barely matters: 1024 cycles already resolves the period to a few
// ppm (40 MHz counted over ~7.5 ms), and empirically increasing it does not reduce
// drift — drift is temperature-driven, not measurement-noise-driven (see the
// esp32-s3 timekeeping analysis). So use the IDF default 1024 (~7 ms) rather than
// paying ~55 ms/sleep for 8192 that buys nothing.
constexpr uint32_t RTC_CAL_CYCLES = 1024;

// Last slow-clock period measured at sleep entry (µs). Doubles as a temperature
// proxy in the diagnostic log — the RC period tracks die temperature ~linearly.
double lastMeasuredPeriodUs = 0.0;

// Re-measure the RTC slow-clock period and install it, so the native deep-sleep
// timekeeping uses a precise, temperature-current calibration for this sleep.
void recalibrateRtcSlowClock() {
  // RTC_CAL_RTC_MUX = whatever slow clock is currently selected (the 150 kHz RC here).
  const uint32_t period = rtc_clk_cal(RTC_CAL_RTC_MUX, RTC_CAL_CYCLES);
  if (period == 0) {
    LOG_ERR("RTC_CAL", "rtc_clk_cal returned 0; keeping existing calibration");
    return;
  }
  esp_clk_slowclk_cal_set(period);
  // period is a fixed-point value with RTC_CLK_CAL_FRACT (19) fractional bits, in us.
  lastMeasuredPeriodUs = period / (double)(1u << RTC_CLK_CAL_FRACT);
  LOG_DBG("RTC_CAL", "Recalibrated RTC slow clock: period=%.4f us (%u cycles)", lastMeasuredPeriodUs, RTC_CAL_CYCLES);
}

// --- TEMP on-SD diagnostic log ------------------------------------------------
// One JSON object per line at each sleep/wake/sync so the clock can be
// characterized untethered (terminal misses the ~355 ms wake logs). Analyze
// later, then remove. Capped so it can't fill the card.
constexpr char DIAG_FILE[] = "/.crosspoint/rtc_diag.jsonl";
constexpr uint64_t DIAG_MAX_BYTES = 512ULL * 1024;

void appendDiag(JsonDocument& d) {
  d["ms"] = (uint32_t)millis();
  d["dev"] = (int64_t)::time(nullptr);  // device clock at log time
  EspFsFile f = Storage.open(DIAG_FILE, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) {
    return;
  }
  if (f.fileSize64() <= DIAG_MAX_BYTES) {
    String line;
    serializeJson(d, line);
    line += '\n';
    f.write(reinterpret_cast<const uint8_t*>(line.c_str()), line.length());
  }
  f.close();
}

void diagWake(const char* action, double driftRatio, int64_t rtcElapsed, int64_t correction) {
  JsonDocument d;
  d["ev"] = "wake";
  d["reset"] = dbgWakeResetReason;
  d["rtcValid"] = dbgWakeRtcValid;
  d["sleepStart"] = dbgWakeSleepStart;
  d["rtcWake"] = rtcWakeTime;
  d["rtcElapsed"] = rtcElapsed;
  d["driftRatio"] = driftRatio;
  d["action"] = action;
  d["correction"] = correction;
  appendDiag(d);
}

void diagSync(const char* outcome, int64_t ntp, int64_t bracketStart, int64_t rtcWake, int64_t awake, int64_t accWake,
              int64_t rtcSleepDur, int64_t actualSleepDur, double cycleRatio, double ratioBefore, double ratioAfter) {
  JsonDocument d;
  d["ev"] = "sync";
  d["outcome"] = outcome;
  d["ntp"] = ntp;
  d["wakeRtcValid"] = dbgWakeRtcValid;
  d["wakeReset"] = dbgWakeResetReason;
  d["sleepStart"] = bracketStart;
  d["rtcWake"] = rtcWake;
  d["awake"] = awake;
  d["accWake"] = accWake;
  // clock error at wake (s); +fast / -slow. THE per-sleep accuracy metric.
  d["driftAtWake"] = (rtcWake != 0 && accWake != 0) ? (rtcWake - accWake) : 0;
  d["rtcSleepDur"] = rtcSleepDur;
  d["actualSleepDur"] = actualSleepDur;
  d["cycleRatio"] = cycleRatio;
  d["ratioBefore"] = ratioBefore;
  d["ratioAfter"] = ratioAfter;
  appendDiag(d);
}
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
  // A1+A2: refresh the slow-clock calibration at the current temperature before
  // sleeping. Independent of NTP/clock state — it only makes the native RTC keep
  // better time across the coming sleep.
  recalibrateRtcSlowClock();

  time_t now;
  ::time(&now);

  if (!HalClock::isSynced()) {
    LOG_DBG("RTC_CAL", "Time not valid, skipping sleep entry record");
    return;
  }

  // Authoritative bracket start in RTC_NOINIT — survives the deep sleep reliably,
  // unlike an SD write flushed right before power-down.
  rtcSleepStartEpoch = static_cast<int64_t>(now);
  rtcSleepStartMagic = SLEEP_START_MAGIC;

  // Still mirror to SD (harmless; keeps the file's view coherent for debugging),
  // but wake reads the RTC_NOINIT copy above.
  CalibrationData data;
  loadCalibration(data);  // Load existing data to preserve driftRatio and lastNtpTime
  data.sleepStartTime = static_cast<int64_t>(now);
  saveCalibration(data);
  LOG_DBG("RTC_CAL", "Recorded sleep entry at %lld", (long long)rtcSleepStartEpoch);

  {
    JsonDocument d;
    d["ev"] = "sleep";
    d["sleepStart"] = rtcSleepStartEpoch;
    d["period_us"] = lastMeasuredPeriodUs;  // temperature proxy
    d["driftRatio"] = data.driftRatio;
    appendDiag(d);
  }
}

void TimeUtil::correctTimeOnWake() {
  // Long-term learner state (driftRatio/lastNtpTime) from SD; a read failure is
  // non-fatal — it just means no correction this wake, but we can still capture a
  // wake reference so the learner gets a sample on the next sync.
  CalibrationData data;
  loadCalibration(data);

  // Bracket start comes from RTC_NOINIT (survives the deep sleep), NOT the SD copy.
  const bool haveRtcSleepStart = (rtcSleepStartMagic == SLEEP_START_MAGIC);
  const int64_t sleepStart = haveRtcSleepStart ? rtcSleepStartEpoch : (int64_t)0;
  // TEMP DIAGNOSTIC: stash for re-print at the next sync (this log is lost — see
  // the globals' comment). resetReason 8 = ESP_RST_DEEPSLEEP.
  dbgWakeRan = true;
  dbgWakeRtcValid = haveRtcSleepStart;
  dbgWakeSleepStart = sleepStart;
  dbgWakeResetReason = (int)esp_reset_reason();
  LOG_INF("RTC_CAL", "onWake: rtcNoinitValid=%d sleepStart=%lld resetReason=%d driftRatio=%.6f", haveRtcSleepStart,
          (long long)sleepStart, dbgWakeResetReason, data.driftRatio);
  // Consume it: each sleep-start pairs with exactly one wake. Invalidate so a later
  // software restart (which never called recordSleepEntry) can't reuse a stale start
  // and fabricate a bogus "sleep" spanning the previous awake session.
  rtcSleepStartMagic = 0;
  if (sleepStart == 0) {
    LOG_DBG("RTC_CAL", "No sleep start time, skipping correction");
    diagWake("skip_nostart", data.driftRatio, 0, 0);
    return;
  }

  time_t now;
  ::time(&now);
  const int64_t rtcNow = static_cast<int64_t>(now);
  const int64_t rtcElapsed = rtcNow - sleepStart;

  // Capture the wake reference for onNtpSynced(): the raw RTC time, the resolved
  // sleep start, and a crystal-based monotonic instant (Item D) so the awake
  // interval before the next sync can be removed from the drift sample.
  rtcWakeTime = rtcNow;
  wakeMonotonicUs = esp_timer_get_time();
  bracketSleepStart = sleepStart;

  if (data.driftRatio == 1.0) {
    LOG_DBG("RTC_CAL", "No drift data yet, skipping correction (captured rtcWakeTime=%lld)", rtcWakeTime);
    diagWake("skip_nodrift", data.driftRatio, rtcElapsed, 0);
    return;
  }

  // A3 fail-safe: refuse to apply a stale (or malformed) drift ratio. rtcNow is
  // the uncorrected RTC time, which is more than accurate enough to judge an age
  // measured in days.
  const int64_t calibrationAge = rtcNow - data.driftRatioUpdatedAt;
  if (data.driftRatioUpdatedAt <= 0 || calibrationAge > MAX_CALIBRATION_AGE_SECONDS) {
    LOG_INF("RTC_CAL", "Drift ratio stale (age %lldh, ratio=%.6f); skipping correction, using raw RTC",
            (long long)(calibrationAge / 3600), data.driftRatio);
    diagWake("skip_stale", data.driftRatio, rtcElapsed, 0);
    return;
  }

  if (rtcElapsed < MIN_ELAPSED_SECONDS) {
    LOG_DBG("RTC_CAL", "Sleep too short (%llds), skipping correction", rtcElapsed);
    diagWake("skip_short", data.driftRatio, rtcElapsed, 0);
    return;
  }

  // Correct: if RTC runs fast (ratio > 1), real elapsed time is less than RTC elapsed
  const int64_t correctedElapsed = static_cast<int64_t>(static_cast<double>(rtcElapsed) / data.driftRatio);
  const int64_t correctedTime = sleepStart + correctedElapsed;
  const int64_t correction = correctedElapsed - rtcElapsed;

  struct timeval tv;
  tv.tv_sec = static_cast<time_t>(correctedTime);
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);

  LOG_INF("RTC_CAL", "Corrected %llds of drift (slept %llds, ratio=%.6f)",
          correction, rtcElapsed, data.driftRatio);
  diagWake("applied", data.driftRatio, rtcElapsed, correction);
}

bool TimeUtil::syncAndCalibrate() {
  if (!HalClock::syncNtp()) {
    return false;
  }
  onNtpSynced();
  return true;
}

void TimeUtil::onNtpSynced() {
  if (!HalClock::isSynced()) {
    LOG_DBG("RTC_CAL", "Time not valid after NTP sync, skipping calibration");
    return;
  }

  time_t now;
  ::time(&now);
  const int64_t actualNow = static_cast<int64_t>(now);

  // TEMP DIAGNOSTIC: replay what correctTimeOnWake saw (its own log is lost in the
  // ~300 ms USB-CDC reconnect window after a deep-sleep wake).
  LOG_INF("RTC_CAL", "onSync: wakeRan=%d wakeRtcValid=%d wakeSleepStart=%lld wakeReset=%d rtcWakeTime=%lld",
          dbgWakeRan, dbgWakeRtcValid, (long long)dbgWakeSleepStart, dbgWakeResetReason, (long long)rtcWakeTime);

  CalibrationData data;
  loadCalibration(data);  // OK if file doesn't exist yet — defaults are fine

  if (data.lastNtpTime == 0) {
    // First sync ever — just record baseline to measure the next interval against.
    data.lastNtpTime = actualNow;
    saveCalibration(data);
    LOG_INF("RTC_CAL", "NTP synced, recorded baseline at %lld", actualNow);
    diagSync("baseline", actualNow, bracketSleepStart, rtcWakeTime, 0, 0, 0, 0, 0, data.driftRatio, data.driftRatio);
    return;
  }

  if (rtcWakeTime == 0 || bracketSleepStart == 0) {
    // No complete sleep→wake bracket this session (first boot, or no sleep since
    // the last consumed sample) — record baseline and wait for the next cycle.
    data.lastNtpTime = actualNow;
    saveCalibration(data);
    LOG_INF("RTC_CAL", "No pre-correction RTC time available, updating baseline");
    diagSync("no_bracket", actualNow, bracketSleepStart, rtcWakeTime, 0, 0, 0, 0, 0, data.driftRatio, data.driftRatio);
    return;
  }

  // Three known timestamps:
  //   sleepStartTime: NTP-accurate time when we entered sleep
  //   rtcWakeTime:    raw RTC time when we woke (before any correction)
  //   actualNow:      NTP-accurate time right now
  //
  // Item D: actualNow is when we synced, which is later than the wake — the user
  // read for a while before Wi-Fi connected. That awake gap is measured accurately
  // by the crystal (esp_timer), so subtract it to recover the NTP-accurate time AT
  // THE WAKE. Both spans then cover only the sleep, and the ratio is unbiased.
  const int64_t awakeSeconds = (esp_timer_get_time() - wakeMonotonicUs) / 1000000;
  const int64_t accurateWakeTime = actualNow - awakeSeconds;
  const int64_t actualSleepDuration = accurateWakeTime - bracketSleepStart;
  const int64_t rtcSleepDuration = rtcWakeTime - bracketSleepStart;

  if (rtcSleepDuration < MIN_ELAPSED_SECONDS || actualSleepDuration < MIN_ELAPSED_SECONDS) {
    data.lastNtpTime = actualNow;
    saveCalibration(data);
    LOG_DBG("RTC_CAL", "Sleep too short (rtc=%llds, actual=%llds), updating baseline",
            rtcSleepDuration, actualSleepDuration);
    diagSync("reject_short", actualNow, bracketSleepStart, rtcWakeTime, awakeSeconds, accurateWakeTime, rtcSleepDuration,
             actualSleepDuration, 0, data.driftRatio, data.driftRatio);
    rtcWakeTime = 0;
    wakeMonotonicUs = 0;
    bracketSleepStart = 0;
    return;
  }

  // Drift ratio for this sleep cycle: how many RTC seconds per real second
  const double cycleRatio = static_cast<double>(rtcSleepDuration) / static_cast<double>(actualSleepDuration);

  // Item E: drop implausible samples rather than let them poison the EMA.
  if (cycleRatio < MIN_PLAUSIBLE_RATIO || cycleRatio > MAX_PLAUSIBLE_RATIO) {
    LOG_INF("RTC_CAL", "Implausible drift sample %.6f (rtc=%llds, actual=%llds); discarding", cycleRatio,
            rtcSleepDuration, actualSleepDuration);
    diagSync("reject_implausible", actualNow, bracketSleepStart, rtcWakeTime, awakeSeconds, accurateWakeTime,
             rtcSleepDuration, actualSleepDuration, cycleRatio, data.driftRatio, data.driftRatio);
    data.lastNtpTime = actualNow;
    saveCalibration(data);
    rtcWakeTime = 0;
    wakeMonotonicUs = 0;
    bracketSleepStart = 0;
    return;
  }

  // Smooth with exponential moving average
  const double oldRatio = data.driftRatio;
  const double newRatio = SMOOTHING_ALPHA * cycleRatio + (1.0 - SMOOTHING_ALPHA) * data.driftRatio;

  LOG_INF("RTC_CAL", "Cycle drift: %.6f, smoothed: %.6f -> %.6f (slept %llds)",
          cycleRatio, data.driftRatio, newRatio, actualSleepDuration);

  data.driftRatio = newRatio;
  data.lastNtpTime = actualNow;
  data.driftRatioUpdatedAt = actualNow;  // A3: stamp freshness so wake-time correction can trust it
  saveCalibration(data);
  diagSync("accepted", actualNow, bracketSleepStart, rtcWakeTime, awakeSeconds, accurateWakeTime, rtcSleepDuration,
           actualSleepDuration, cycleRatio, oldRatio, newRatio);

  // Reset for next cycle
  rtcWakeTime = 0;
  wakeMonotonicUs = 0;
  bracketSleepStart = 0;
}
