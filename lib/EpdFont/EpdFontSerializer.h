#pragma once
#include <cstddef>
#include <cstdint>

#include "EpdFont.h"
#include "HalStorage.h"

#ifdef ENABLE_CUSTOM_FONTS

/**
 * @brief Serializes/deserializes EpdFont data to/from binary .epdfont files.
 *
 * File format (little-endian, sequential):
 *   uint32  magic     = 0x46445045  ('EPDF')
 *   uint8   version   = 1
 *   uint8   advanceY
 *   int32   ascender
 *   int32   descender
 *   uint8   is2Bit
 *   uint32  intervalCount
 *   [N × EpdUnicodeInterval]
 *   uint32  glyphCount
 *   [N × EpdGlyph]
 *   uint16  kernLeftEntryCount
 *   uint16  kernRightEntryCount
 *   uint8   kernLeftClassCount
 *   uint8   kernRightClassCount
 *   [N × EpdKernClassEntry left]
 *   [N × EpdKernClassEntry right]
 *   [kernLeftClassCount × kernRightClassCount × int8_t matrix]
 *   uint32  ligaturePairCount
 *   [N × EpdLigaturePair]
 *   uint32  bitmapSize
 *   [bitmapSize × uint8_t]
 */
class EpdFontSerializer {
 public:
  static constexpr uint32_t MAGIC = 0x46445045;  // 'EPDF'
  static constexpr uint8_t VERSION = 1;

  /**
   * @brief Serialize an EpdFont to an open file.
   * @param font Font to serialize (must have valid data pointer)
   * @param glyphCount Number of glyphs in font->data->glyph[]
   * @param file Open file for writing
   * @return true on success
   */
  static bool serialize(const EpdFont* font, size_t glyphCount, EspFsFile& file);

  /**
   * @brief Load an .epdfont file from SD card into a single PSRAM block.
   *
   * Returns a malloc'd EpdFont* whose entire data (including bitmaps, glyphs,
   * intervals, kern tables and ligature pairs) is contained in ONE contiguous
   * PSRAM allocation. Call EpdFontSerializer::freeFont() when done.
   *
   * @param path SD-card path to the .epdfont file
   * @return Heap-allocated EpdFont* or nullptr on failure
   */
  static EpdFont* loadFromFile(const char* path);

  /**
   * @brief Free a font previously allocated by loadFromFile().
   */
  static void freeFont(EpdFont* font);
};

#endif  // ENABLE_CUSTOM_FONTS
