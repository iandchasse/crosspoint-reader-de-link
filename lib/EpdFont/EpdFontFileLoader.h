#pragma once

#ifdef ENABLE_CUSTOM_FONTS

#include <Arduino.h>

#include "EpdFontFamily.h"

/**
 * @brief Manages the SD-cached custom font family.
 *
 * Stores font family path and per-size point values in NVS. On first render,
 * lazily loads the appropriate .epdfont files into PSRAM and registers them
 * with the renderer. Only ONE size family is held in RAM at a time.
 *
 * Directory layout on SD card:
 *   /.fonts/<family>/<pt>_regular.epdfont
 *   /.fonts/<family>/<pt>_bold.epdfont
 *   /.fonts/<family>/<pt>_italic.epdfont
 *   /.fonts/<family>/<pt>_bolditalic.epdfont
 */
class EpdFontFileLoader {
 public:
  enum SizeSlot { SMALL = 0, MEDIUM = 1, LARGE = 2, EXTRA_LARGE = 3, SIZE_SLOT_COUNT };

  // Stable font IDs for SD-cached custom fonts, one per size slot.
  // Defined here (in a lib header) so both lib/ and src/ code can use them
  // without crossing the src/fontIds.h boundary.
  static constexpr int FONT_ID_SMALL = -2000000001;
  static constexpr int FONT_ID_MEDIUM = -2000000002;
  static constexpr int FONT_ID_LARGE = -2000000003;
  static constexpr int FONT_ID_EXTRA_LARGE = -2000000004;

  /**
   * @brief Call once at boot (after SD mounted). Reads NVS config.
   */
  static void init();

  /**
   * @brief Returns true if a valid custom font family is configured.
   */
  static bool isAvailable();

  /**
   * @brief Returns the name (family folder name) of the configured custom font.
   * Returns empty string if none configured.
   */
  static String getFamilyName();

  /**
   * @brief Returns the point size for a given size slot.
   */
  static int getPointSize(SizeSlot slot);

  /**
   * @brief Lazy-load the family for a given size slot. Returns nullptr if unavailable.
   * On success, the returned EpdFontFamily is valid until clearCache() is called.
   */
  static EpdFontFamily* getFamily(SizeSlot slot);

  /**
   * @brief Free any cached PSRAM font data and clear the active cache.
   * Call when settings change or when evicting to free RAM.
   */
  static void clearCache();

  /**
   * @brief Persist a new custom font family configuration to NVS.
   * @param basePath   SD path to the .fonts/<family> directory (e.g. "/.fonts/roboto")
   * @param pts        Array of 4 point sizes [S, M, L, XL]
   */
  static bool setFamily(const String& basePath, const int pts[SIZE_SLOT_COUNT]);

  /**
   * @brief Clear NVS + cache. Removes custom font configuration entirely.
   */
  static void clearFamily();

  /**
   * @brief Get the stable font ID for a given size slot.
   */
  static int getFontId(SizeSlot slot);

 private:
  static EpdFontFamily* cachedFamily;
  static SizeSlot cachedSlot;
  static bool cacheValid;
  static String familyBasePath;
  static int customPt[SIZE_SLOT_COUNT];
  static bool initialized;

  static EpdFont* loadStyle(SizeSlot slot, const char* styleSuffix);
  static void freeCachedFamily();
};

#endif  // ENABLE_CUSTOM_FONTS
