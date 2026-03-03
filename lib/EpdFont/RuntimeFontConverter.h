#pragma once
#include <cstddef>
#include <cstdint>

#include "EpdFont.h"
#include "HalStorage.h"

#ifdef ENABLE_CUSTOM_FONTS

class RuntimeFontConverter {
 public:
  // Converts a true type font TTF file into EpdFont (single style) allocated in PSRAM.
  // Returns nullptr if memory allocation fails or TTF is invalid.
  static EpdFont* generateEpdFontFromBuffer(const uint8_t* ttfBuffer, size_t ttfSize, int sizePt, bool is2Bit);

  // Helper to load a single font style from a path.
  static EpdFont* generateEpdFontFromPath(const char* sdPath, int sizePt, bool is2Bit, const char* charsets = nullptr);

  // Generate a font from TTF, serialize it to an .epdfont file, then free
  // the PSRAM immediately. The caller's SD directory must exist.
  // Returns true on success.
  static bool generateAndSaveToFile(const char* sdTtfPath, int sizePt, bool is2Bit, const char* outEpdFontPath,
                                    const char* charsets = nullptr);

  // Helper cleanup function for an individual EpdFont.
  static void freeEpdFont(EpdFont* font);

  // Returns the number of glyphs in a font data block (needed for serialization).
  static size_t countGlyphs(const EpdFont* font);

 private:
  // Internal implementation for rasterization.
  static EpdFont* generateEpdFont(const uint8_t* ttfBuffer, size_t ttfSize, int sizePt, bool is2Bit);
};

#endif  // ENABLE_CUSTOM_FONTS
