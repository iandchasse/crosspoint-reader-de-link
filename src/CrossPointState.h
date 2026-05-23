#pragma once
#include <cstdint>
#include <string>

enum class BootWakeButton : uint8_t { None = 0, PageFwd = 1, PageBack = 2, Menu = 3, Home = 4 };

class CrossPointState {
  // Static instance
  static CrossPointState instance;

 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
  uint8_t readerActivityLoadCount = 0;
  uint8_t pagesUntilFullRefresh = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;
  BootWakeButton pendingWakeAction = BootWakeButton::None;

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
  ~CrossPointState() = default;

  // Get singleton instance
  static CrossPointState& getInstance() { return instance; }

  bool saveToFile() const;

  bool loadFromFile();

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access settings
#define APP_STATE CrossPointState::getInstance()
