#pragma once
#include <cstddef>
#include <cstdint>

#include "EpdFont.h"
#include "HalStorage.h"

#ifdef ENABLE_CUSTOM_FONTS

class RuntimeFontConverter {
 public:
  /// Converts a TTF file on storage to an .epdfont file (streaming, low RAM), then
  /// returns an EpdStreamFont that lazily reads bitmap data from that file.
  /// Uses TtfTableLoader internally — peak RAM usage ~80 KB, no PSRAM required.
  static EpdFont* generateEpdFontFromPath(const char* sdPath, int sizePt, bool is2Bit, const char* charsets = nullptr);

  /// Generate a font from TTF and save to an .epdfont file.
  /// Uses windowed TtfTableLoader — no full-file TTF allocation.
  /// Returns true on success.
  static bool generateAndSaveToFile(const char* sdTtfPath, int sizePt, bool is2Bit, const char* outEpdFontPath,
                                    const char* charsets = nullptr);

  /// Helper cleanup function for an individual EpdFont.
  static void freeEpdFont(EpdFont* font);

  /// Returns the number of glyphs in a font data block (needed for serialization).
  static size_t countGlyphs(const EpdFont* font);
};

#endif  // ENABLE_CUSTOM_FONTS
