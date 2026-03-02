#pragma once
#include <Arduino.h>

#ifdef ENABLE_CUSTOM_FONTS
#include "EpdFontFamily.h"

// CustomFontManager handles saving and loading the dynamic TTF font setting
// from NVS, and acts as the global provider for the dynamically generated EpdFontFamily.
class CustomFontManager {
 public:
  // Called on boot (after SD card is initialized) to check NVS, and
  // optionally generate the PSRAM font if one was configured.
  static void init();

  // Returns the loaded custom font if it exists and was successfully generated, else nullptr.
  static EpdFontFamily* getActiveCustomFont();

  // Sets a new custom font, saves its path/size to NVS, and generates it into PSRAM.
  // Returns true if generation was successful.
  static bool setActiveCustomFont(const String& ttFPath, int sizePt, bool is2Bit);

  // Clears the current custom font, freeing the PSRAM and erasing from NVS.
  static void clearCustomFont();

  /**
   * @brief Returns the total PSRAM usage of the currently loaded custom font family.
   */
  static size_t getTotalRamUsage();

  // Helper flags
  static bool hasCustomFont() { return customFontFamily != nullptr; }

 private:
  static EpdFontFamily* customFontFamily;
};

#endif  // ENABLE_CUSTOM_FONTS
