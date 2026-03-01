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
  static EpdFont* generateEpdFontFromPath(const char* sdPath, int sizePt, bool is2Bit);

  // Helper cleanup function for an individual EpdFont.
  static void freeEpdFont(EpdFont* font);

 private:
  // Internal implementation for rasterization.
  static EpdFont* generateEpdFont(const uint8_t* ttfBuffer, size_t ttfSize, int sizePt, bool is2Bit);
};

#endif  // ENABLE_CUSTOM_FONTS
