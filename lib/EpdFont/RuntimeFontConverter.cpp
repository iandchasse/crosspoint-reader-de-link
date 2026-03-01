#include "RuntimeFontConverter.h"

#ifdef ENABLE_CUSTOM_FONTS

#define STB_TRUETYPE_IMPLEMENTATION
// stb_truetype doesn't use standard libs for much other than math/string.
// We should make sure it doesn't blow up the flash footprint further but ESP32 handles it.
#include <FS.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cmath>
#include <vector>

#include "esp_heap_caps.h"
#include "stb_truetype.h"

struct EpdUnicodeIntervalBase {
  uint32_t first;
  uint32_t last;
};

// Based on fontconvert.py basic ranges
static const EpdUnicodeIntervalBase baseIntervals[] = {
    {0x0000, 0x007F},                                      // Basic Latin
    {0x0080, 0x00FF},                                      // Latin-1 Supplement
    {0x0100, 0x017F},                                      // Latin Extended-A
    {0x01A0, 0x01A1},                                      // Latin Extended-B
    {0x01AF, 0x01B0}, {0x01C4, 0x021F}, {0x0300, 0x036F},  // Combining Diacritical Marks
    {0x0400, 0x04FF},                                      // Cyrillic
    {0x1EA0, 0x1EF9},                                      // Vietnamese Extended
    {0x2000, 0x20CF},                                      // General Punctuation & Currency
    {0x2190, 0x21FF},                                      // Arrows
    {0x2200, 0x22FF},                                      // Math Operators
    {0xFB00, 0xFB06},                                      // Alphabetic Presentation Forms
    {0xFFFD, 0xFFFD}                                       // Replacement
};

EpdFont* RuntimeFontConverter::generateEpdFontFromPath(const char* sdPath, int sizePt, bool is2Bit) {
  LOG_INF("RFC", "Loading font via HalStorage: %s (size: %d, 2-bit: %s)", sdPath, sizePt, is2Bit ? "yes" : "no");

  EspFsFile file;
  if (!Storage.openFileForRead("RFC", sdPath, file)) {
    LOG_ERR("RFC", "Failed to open font file: %s", sdPath);
    return nullptr;
  }

  size_t size = file.size();
  LOG_DBG("RFC", "File size: %zu bytes", size);

  if (size == 0 || size > 5 * 1024 * 1024) {  // Arbitrary 5MB cap
    LOG_ERR("RFC", "Invalid font file size: %zu", size);
    file.close();
    return nullptr;
  }

  // Allocate TTF to PSRAM temporarily
  uint8_t* ttfBuffer = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (!ttfBuffer) {
    file.close();
    return nullptr;
  }

  file.read(ttfBuffer, size);
  file.close();

  LOG_DBG("RFC", "Rasterizing font...");
  EpdFont* font = generateEpdFont(ttfBuffer, size, sizePt, is2Bit);

  if (font) {
    LOG_INF("RFC", "Successfully generated EpdFont");
  } else {
    LOG_ERR("RFC", "Failed to rasterize font");
  }

  heap_caps_free(ttfBuffer);
  return font;
}

EpdFont* RuntimeFontConverter::generateEpdFontFromBuffer(const uint8_t* ttfBuffer, size_t ttfSize, int sizePt,
                                                         bool is2Bit) {
  return generateEpdFont(ttfBuffer, ttfSize, sizePt, is2Bit);
}

EpdFont* RuntimeFontConverter::generateEpdFont(const uint8_t* ttfBuffer, size_t ttfSize, int sizePt, bool is2Bit) {
  stbtt_fontinfo fontInfo;
  if (!stbtt_InitFont(&fontInfo, ttfBuffer, stbtt_GetFontOffsetForIndex(ttfBuffer, 0))) {
    LOG_ERR("RFC", "stbtt_InitFont failed");
    return nullptr;
  }

  // fontconvert.py uses 150 DPI. sizePt at 150 DPI = sizePt * 150 / 72 pixels.
  float ppem = sizePt * 150.0f / 72.0f;
  float scale = stbtt_ScaleForMappingEmToPixels(&fontInfo, ppem);

  int ascent_units, descent_units, lineGap_units;
  stbtt_GetFontVMetrics(&fontInfo, &ascent_units, &descent_units, &lineGap_units);

  int ascent = roundf(ascent_units * scale);
  int descent = roundf(descent_units * scale);
  int advanceY = ascent - descent + roundf(lineGap_units * scale);

  std::vector<EpdUnicodeInterval> intervalIndices;
  size_t total_bitmap_size = 0;
  size_t valid_glyph_count = 0;

  // First pass: sizing and counting
  for (size_t i = 0; i < sizeof(baseIntervals) / sizeof(baseIntervals[0]); ++i) {
    bool in_range = false;
    uint32_t current_first = 0;
    uint32_t current_offset = valid_glyph_count;

    for (uint32_t cp = baseIntervals[i].first; cp <= baseIntervals[i].last; ++cp) {
      int glyphIndex = stbtt_FindGlyphIndex(&fontInfo, cp);
      if (glyphIndex > 0) {
        if (!in_range) {
          current_first = cp;
          current_offset = valid_glyph_count;
          in_range = true;
        }

        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&fontInfo, glyphIndex, scale, scale, &x0, &y0, &x1, &y1);
        int w = x1 - x0;
        int h = y1 - y0;

        // Linear packing matching fontconvert.py
        int bits = w * h * (is2Bit ? 2 : 1);
        total_bitmap_size += (bits + 7) / 8;
        valid_glyph_count++;
      } else {
        if (in_range) {
          intervalIndices.push_back({current_first, cp - 1, current_offset});
          in_range = false;
        }
      }
    }
    if (in_range) {
      intervalIndices.push_back({current_first, baseIntervals[i].last, current_offset});
    }
  }

  if (valid_glyph_count == 0 || total_bitmap_size > 3 * 1024 * 1024) {
    LOG_ERR("RFC", "Font selection invalid or too large (%zu bytes)", total_bitmap_size);
    return nullptr;
  }

  // Allocate ONE giant block in PSRAM
  auto align4 = [](size_t s) { return (s + 3) & ~3; };
  size_t szEpdFont = align4(sizeof(EpdFont));
  size_t szEpdFontData = align4(sizeof(EpdFontData));
  size_t szIntervals = align4(sizeof(EpdUnicodeInterval) * intervalIndices.size());
  size_t szGlyphs = align4(sizeof(EpdGlyph) * valid_glyph_count);
  size_t total_alloc = szEpdFont + szEpdFontData + szIntervals + szGlyphs + total_bitmap_size;

  uint8_t* memBlock = (uint8_t*)heap_caps_malloc(total_alloc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!memBlock) {
    LOG_ERR("RFC", "PSRAM alloc failed for %zu bytes", total_alloc);
    return nullptr;
  }
  memset(memBlock, 0, total_alloc);

  uint8_t* ptr = memBlock;
  EpdFont* epdFont = (EpdFont*)ptr;
  ptr += szEpdFont;
  EpdFontData* fontData = (EpdFontData*)ptr;
  ptr += szEpdFontData;
  EpdUnicodeInterval* outIntervals = (EpdUnicodeInterval*)ptr;
  ptr += szIntervals;
  EpdGlyph* outGlyphs = (EpdGlyph*)ptr;
  ptr += szGlyphs;
  uint8_t* outBitmap = ptr;

  // Wire up structs
  new (epdFont) EpdFont(fontData);

  fontData->bitmap = outBitmap;
  fontData->glyph = outGlyphs;
  fontData->intervals = outIntervals;
  fontData->intervalCount = intervalIndices.size();
  fontData->is2Bit = is2Bit;
  fontData->advanceY = std::min(255, std::max(0, advanceY));
  fontData->ascender = ascent;
  fontData->descender = descent;
  fontData->totalAllocatedSize = total_alloc;

  fontData->groups = nullptr;
  fontData->groupCount = 0;
  fontData->kernLeftClasses = nullptr;
  fontData->kernRightClasses = nullptr;
  fontData->kernMatrix = nullptr;
  fontData->kernLeftEntryCount = 0;
  fontData->kernRightEntryCount = 0;
  fontData->kernLeftClassCount = 0;
  fontData->kernRightClassCount = 0;
  fontData->ligaturePairs = nullptr;
  fontData->ligaturePairCount = 0;

  // Copy intervals
  for (size_t i = 0; i < intervalIndices.size(); ++i) {
    outIntervals[i] = intervalIndices[i];
  }

  // Second pass: Rasterize and pack
  size_t current_glyph_idx = 0;
  size_t current_bitmap_offset = 0;

  for (size_t i = 0; i < intervalIndices.size(); ++i) {
    for (uint32_t cp = intervalIndices[i].first; cp <= intervalIndices[i].last; ++cp) {
      int glyphIndex = stbtt_FindGlyphIndex(&fontInfo, cp);
      if (glyphIndex <= 0) continue;

      int x0, y0, x1, y1;
      int advanceWidth, leftSideBearing;
      stbtt_GetGlyphHMetrics(&fontInfo, glyphIndex, &advanceWidth, &leftSideBearing);
      stbtt_GetGlyphBitmapBox(&fontInfo, glyphIndex, scale, scale, &x0, &y0, &x1, &y1);

      int w = x1 - x0;
      int h = y1 - y0;
      int advanceX_px = roundf(advanceWidth * scale);

      EpdGlyph& g = outGlyphs[current_glyph_idx];
      g.width = w;
      g.height = h;
      g.advanceX = std::min(255, std::max(0, advanceX_px));
      g.left = x0;
      g.top = -y0;
      g.dataOffset = current_bitmap_offset;

      if (w > 0 && h > 0) {
        std::vector<uint8_t> tempMask(w * h);
        stbtt_MakeGlyphBitmap(&fontInfo, tempMask.data(), w, h, w, scale, scale, glyphIndex);

        int pixelCount = w * h;
        uint8_t out_byte = 0;
        int bit_in_byte = 0;

        for (int p = 0; p < pixelCount; ++p) {
          uint8_t alpha = tempMask[p];
          uint8_t val = 0;
          if (is2Bit) {
            if (alpha >= 192)
              val = 3;
            else if (alpha >= 128)
              val = 2;
            else if (alpha >= 64)
              val = 1;
            out_byte = (out_byte << 2) | val;
            bit_in_byte += 2;
          } else {
            if (alpha >= 128) val = 1;
            out_byte = (out_byte << 1) | val;
            bit_in_byte += 1;
          }

          if (bit_in_byte == 8) {
            outBitmap[current_bitmap_offset++] = out_byte;
            out_byte = 0;
            bit_in_byte = 0;
          }
        }
        if (bit_in_byte > 0) {
          out_byte <<= (8 - bit_in_byte);
          outBitmap[current_bitmap_offset++] = out_byte;
        }
        g.dataLength = (pixelCount * (is2Bit ? 2 : 1) + 7) / 8;
      } else {
        g.dataLength = 0;
      }
      current_glyph_idx++;
    }
  }

  LOG_INF("RFC", "Custom font generated: %zu glyphs, %zu bytes bitmap", valid_glyph_count, current_bitmap_offset);

  // Debug: Print LUT (EpdGlyph data) to match fontconvert.py style
  //   LOG_INF("RFC", "Dumping LUT (EpdGlyph data):");
  //   LOG_INF("RFC", "  { Offset,  W,   H, Adv,   L,   T }");
  //   for (size_t i = 0; i < valid_glyph_count; ++i) {
  //     const EpdGlyph& g = outGlyphs[i];
  //     uint32_t cp = 0;
  //     for (const auto& interval : intervalIndices) {
  //       if (i >= interval.offset && i < interval.offset + (interval.last - interval.first + 1)) {
  //         cp = interval.first + (i - interval.offset);
  //         break;
  //       }
  //     }
  //     LOG_INF("RFC", "  { %6u, %3u, %3u, %3u, %3d, %3d }, // 0x%04X", g.dataOffset, g.width, g.height, g.advanceX,
  //     g.left,
  //             g.top, cp);
  //   }

  return epdFont;
}

void RuntimeFontConverter::freeEpdFont(EpdFont* font) {
  if (!font) return;

  // We packed EpdFont and everything into one block based around EpdFont*.
  heap_caps_free(font);
}

#endif
