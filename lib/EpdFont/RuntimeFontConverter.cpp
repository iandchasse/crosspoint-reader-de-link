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

#include "EpdFontSerializer.h"
#include "EpdStreamFont.h"
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

EpdFont* RuntimeFontConverter::generateEpdFontFromPath(const char* sdPath, int sizePt, bool is2Bit,
                                                       const char* charsets) {
  // To avoid RAM bloat, we use the streaming generator which writes directly to SD,
  // then we return an EpdStreamFont pointing to that file.
  char tmpEpdPath[256];
  snprintf(tmpEpdPath, sizeof(tmpEpdPath), "/tmp/runtime_font_%d.epdfont", sizePt);

  if (!generateAndSaveToFile(sdPath, sizePt, is2Bit, tmpEpdPath, charsets)) {
    LOG_ERR("RFC", "Failed to generate font to %s", tmpEpdPath);
    return nullptr;
  }

  return EpdStreamFont::load(tmpEpdPath);
}

EpdFont* RuntimeFontConverter::generateEpdFontFromBuffer(const uint8_t* ttfBuffer, size_t ttfSize, int sizePt,
                                                         bool is2Bit, const char* charsets) {
  return generateEpdFont(ttfBuffer, ttfSize, sizePt, is2Bit, charsets);
}

EpdFont* RuntimeFontConverter::generateEpdFont(const uint8_t* ttfBuffer, size_t ttfSize, int sizePt, bool is2Bit,
                                               const char* charsets) {
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

  // Helper to decode a UTF-8 string into a subset list of codepoints
  std::vector<uint32_t> customCodepoints;
  if (charsets && charsets[0] != '\0') {
    const uint8_t* p = (const uint8_t*)charsets;
    while (*p) {
      uint32_t c = *p;
      if (c < 0x80) {
        customCodepoints.push_back(c);
        p++;
      } else if ((c & 0xE0) == 0xC0) {
        customCodepoints.push_back(((c & 0x1F) << 6) | (p[1] & 0x3F));
        p += 2;
      } else if ((c & 0xF0) == 0xE0) {
        customCodepoints.push_back(((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
        p += 3;
      } else if ((c & 0xF8) == 0xF0) {
        customCodepoints.push_back(((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F));
        p += 4;
      } else {
        p++;
      }
    }
    std::sort(customCodepoints.begin(), customCodepoints.end());
    auto last = std::unique(customCodepoints.begin(), customCodepoints.end());
    customCodepoints.erase(last, customCodepoints.end());
  }

  // First pass: sizing and counting
  for (size_t i = 0; i < sizeof(baseIntervals) / sizeof(baseIntervals[0]); ++i) {
    bool in_range = false;
    uint32_t current_first = 0;
    uint32_t current_offset = valid_glyph_count;

    for (uint32_t cp = baseIntervals[i].first; cp <= baseIntervals[i].last; ++cp) {
      if (!customCodepoints.empty()) {
        auto it = std::lower_bound(customCodepoints.begin(), customCodepoints.end(), cp);
        if (it == customCodepoints.end() || *it != cp) {
          if (in_range) {
            intervalIndices.push_back({current_first, cp - 1, current_offset});
            in_range = false;
          }
          continue;
        }
      }

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

  // Allocate EpdFont block in internal SRAM (No PSRAM)
  uint8_t* memBlock = (uint8_t*)heap_caps_malloc(total_alloc, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!memBlock) {
    LOG_ERR("RFC", "Failed to allocate %zu bytes in internal RAM for EpdFont", total_alloc);
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
      if (!customCodepoints.empty()) {
        auto it = std::lower_bound(customCodepoints.begin(), customCodepoints.end(), cp);
        if (it == customCodepoints.end() || *it != cp) {
          continue;
        }
      }

      int glyphIndex = stbtt_FindGlyphIndex(&fontInfo, cp);
      if (glyphIndex <= 0) continue;

      int x0, y0, x1, y1;
      int advanceWidth, leftSideBearing;
      stbtt_GetGlyphHMetrics(&fontInfo, glyphIndex, &advanceWidth, &leftSideBearing);
      stbtt_GetGlyphBitmapBox(&fontInfo, glyphIndex, scale, scale, &x0, &y0, &x1, &y1);

      int w = x1 - x0;
      int h = y1 - y0;
      // advanceX is 12.4 fixed-point (shifted 4)
      int advanceX_fp4 = roundf(advanceWidth * scale * 16.0f);

      EpdGlyph& g = outGlyphs[current_glyph_idx];
      g.width = w;
      g.height = h;
      g.advanceX = (uint16_t)std::min(65535, std::max(0, advanceX_fp4));
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

size_t RuntimeFontConverter::countGlyphs(const EpdFont* font) {
  if (!font || !font->data) return 0;
  // Sum glyph counts from all intervals
  size_t total = 0;
  for (uint32_t i = 0; i < font->data->intervalCount; ++i) {
    const EpdUnicodeInterval& iv = font->data->intervals[i];
    total += (iv.last - iv.first + 1);
  }
  return total;
}

static bool writeU8(EspFsFile& f, uint8_t v) { return f.write(&v, 1) == 1; }
static bool writeU16(EspFsFile& f, uint16_t v) { return f.write(reinterpret_cast<const uint8_t*>(&v), 2) == 2; }
static bool writeU32(EspFsFile& f, uint32_t v) { return f.write(reinterpret_cast<const uint8_t*>(&v), 4) == 4; }
static bool writeI32(EspFsFile& f, int32_t v) { return f.write(reinterpret_cast<const uint8_t*>(&v), 4) == 4; }
static bool writeBytes(EspFsFile& f, const void* buf, size_t n) {
  return f.write(reinterpret_cast<const uint8_t*>(buf), n) == (int)n;
}

bool RuntimeFontConverter::generateAndSaveToFile(const char* sdTtfPath, int sizePt, bool is2Bit,
                                                 const char* outEpdFontPath, const char* charsets) {
  LOG_INF("RFC", "Streaming font generation: %s -> %s (pt=%d)", sdTtfPath, outEpdFontPath, sizePt);

  EspFsFile ttfFile;
  if (!Storage.openFileForRead("RFC", sdTtfPath, ttfFile)) {
    LOG_ERR("RFC", "Failed to open TTF file: %s", sdTtfPath);
    return false;
  }
  size_t ttfSize = ttfFile.size();
  if (ttfSize == 0 || ttfSize > 5 * 1024 * 1024) {
    LOG_ERR("RFC", "Invalid font file size: %zu", ttfSize);
    ttfFile.close();
    return false;
  }
  // Allocate TTF buffer in internal SRAM (No PSRAM)
  uint8_t* ttfBuffer = (uint8_t*)heap_caps_malloc(ttfSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!ttfBuffer) {
    LOG_ERR("RFC", "Failed to allocate %zu bytes in internal RAM for TTF buffer", ttfSize);
    ttfFile.close();
    return false;
  }
  ttfFile.read(ttfBuffer, ttfSize);
  ttfFile.close();

  stbtt_fontinfo fontInfo;
  if (!stbtt_InitFont(&fontInfo, ttfBuffer, stbtt_GetFontOffsetForIndex(ttfBuffer, 0))) {
    LOG_ERR("RFC", "stbtt_InitFont failed");
    heap_caps_free(ttfBuffer);
    return false;
  }

  String outPathStr = outEpdFontPath;
  int lastSlash = outPathStr.lastIndexOf('/');
  if (lastSlash > 0) {
    String parentDir = outPathStr.substring(0, lastSlash);
    if (!Storage.exists(parentDir.c_str())) {
      Storage.mkdir(parentDir.c_str());
    }
  }

  EspFsFile outFile;
  if (!Storage.openFileForWrite("RFC", outEpdFontPath, outFile)) {
    LOG_ERR("RFC", "Cannot open output file: %s", outEpdFontPath);
    heap_caps_free(ttfBuffer);
    return false;
  }

  // --- PASS 1: Calculate metrics and count intervals ---
  float ppem = sizePt * 150.0f / 72.0f;
  float scale = stbtt_ScaleForMappingEmToPixels(&fontInfo, ppem);

  int ascent_units, descent_units, lineGap_units;
  stbtt_GetFontVMetrics(&fontInfo, &ascent_units, &descent_units, &lineGap_units);
  int ascent = roundf(ascent_units * scale);
  int descent = roundf(descent_units * scale);
  int advanceY = ascent - descent + roundf(lineGap_units * scale);

  std::vector<uint32_t> customCodepoints;
  if (charsets && charsets[0] != '\0') {
    const uint8_t* p = (const uint8_t*)charsets;
    while (*p) {
      uint32_t c = *p;
      if (c < 0x80) {
        customCodepoints.push_back(c);
        p++;
      } else if ((c & 0xE0) == 0xC0) {
        customCodepoints.push_back(((c & 0x1F) << 6) | (p[1] & 0x3F));
        p += 2;
      } else if ((c & 0xF0) == 0xE0) {
        customCodepoints.push_back(((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
        p += 3;
      } else if ((c & 0xF8) == 0xF0) {
        customCodepoints.push_back(((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F));
        p += 4;
      } else {
        p++;
      }
    }
    std::sort(customCodepoints.begin(), customCodepoints.end());
    auto last = std::unique(customCodepoints.begin(), customCodepoints.end());
    customCodepoints.erase(last, customCodepoints.end());
  }

  std::vector<EpdUnicodeInterval> intervals;
  size_t valid_glyph_count = 0;

  for (size_t i = 0; i < sizeof(baseIntervals) / sizeof(baseIntervals[0]); ++i) {
    bool in_range = false;
    uint32_t current_first = 0;
    uint32_t current_offset = valid_glyph_count;

    for (uint32_t cp = baseIntervals[i].first; cp <= baseIntervals[i].last; ++cp) {
      if (!customCodepoints.empty()) {
        auto it = std::lower_bound(customCodepoints.begin(), customCodepoints.end(), cp);
        if (it == customCodepoints.end() || *it != cp) {
          if (in_range) {
            intervals.push_back({current_first, cp - 1, current_offset});
            in_range = false;
          }
          continue;
        }
      }
      int glyphIndex = stbtt_FindGlyphIndex(&fontInfo, cp);
      if (glyphIndex > 0) {
        if (!in_range) {
          current_first = cp;
          current_offset = valid_glyph_count;
          in_range = true;
        }
        valid_glyph_count++;
      } else {
        if (in_range) {
          intervals.push_back({current_first, cp - 1, current_offset});
          in_range = false;
        }
      }
    }
    if (in_range) {
      intervals.push_back({current_first, baseIntervals[i].last, current_offset});
    }
  }

  if (valid_glyph_count == 0) {
    LOG_ERR("RFC", "No valid glyphs found");
    outFile.close();
    Storage.remove(outEpdFontPath);
    heap_caps_free(ttfBuffer);
    return false;
  }

  // Write Dummy Header (we'll rewrite this at the end)
  writeU32(outFile, EpdFontSerializer::MAGIC);
  writeU8(outFile, EpdFontSerializer::VERSION);
  writeU8(outFile, std::min(255, std::max(0, advanceY)));
  writeI32(outFile, (int32_t)ascent);
  writeI32(outFile, (int32_t)descent);
  writeU8(outFile, is2Bit ? 1 : 0);

  writeU32(outFile, (uint32_t)intervals.size());
  writeBytes(outFile, intervals.data(), sizeof(EpdUnicodeInterval) * intervals.size());

  writeU32(outFile, (uint32_t)valid_glyph_count);
  // We don't have the glyph table yet, so leave blank space for it to overwrite later
  uint32_t glyphsTableOffset = outFile.position();
  for (size_t i = 0; i < valid_glyph_count * sizeof(EpdGlyph); i++) {
    writeU8(outFile, 0);
  }

  // Empty Kern/Ligature counts
  writeU16(outFile, 0);
  writeU16(outFile, 0);
  writeU8(outFile, 0);
  writeU8(outFile, 0);
  writeU32(outFile, 0);

  // Save position of Bitmap Length so we can rewrite it later
  uint32_t bitmapLenOffset = outFile.position();
  writeU32(outFile, 0);

  // --- PASS 2: Rasterize and write bitmaps directly to file ---
  std::vector<EpdGlyph> glyphs(valid_glyph_count);
  size_t current_glyph_idx = 0;
  size_t current_bitmap_offset = 0;

  for (size_t i = 0; i < intervals.size(); ++i) {
    for (uint32_t cp = intervals[i].first; cp <= intervals[i].last; ++cp) {
      if (!customCodepoints.empty()) {
        auto it = std::lower_bound(customCodepoints.begin(), customCodepoints.end(), cp);
        if (it == customCodepoints.end() || *it != cp) continue;
      }
      int glyphIndex = stbtt_FindGlyphIndex(&fontInfo, cp);
      if (glyphIndex <= 0) continue;

      int x0, y0, x1, y1;
      int advanceWidth, leftSideBearing;
      stbtt_GetGlyphHMetrics(&fontInfo, glyphIndex, &advanceWidth, &leftSideBearing);
      stbtt_GetGlyphBitmapBox(&fontInfo, glyphIndex, scale, scale, &x0, &y0, &x1, &y1);

      int w = x1 - x0;
      int h = y1 - y0;
      int advanceX_fp4 = roundf(advanceWidth * scale * 16.0f);

      EpdGlyph& g = glyphs[current_glyph_idx];
      glyphs[current_glyph_idx].width = w;
      glyphs[current_glyph_idx].height = h;
      glyphs[current_glyph_idx].advanceX = (uint16_t)std::min(65535, std::max(0, advanceX_fp4));
      glyphs[current_glyph_idx].left = x0;
      glyphs[current_glyph_idx].top = -y0;
      glyphs[current_glyph_idx].dataOffset = current_bitmap_offset;

      if (w > 0 && h > 0) {
        std::vector<uint8_t> tempMask(w * h);
        stbtt_MakeGlyphBitmap(&fontInfo, tempMask.data(), w, h, w, scale, scale, glyphIndex);

        std::vector<uint8_t> compiledBitmap;

        uint8_t out_byte = 0;
        int bit_in_byte = 0;
        for (int y = 0; y < h; ++y) {
          for (int x = 0; x < w; ++x) {
            uint8_t alpha = tempMask[y * w + x];
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
              compiledBitmap.push_back(out_byte);
              out_byte = 0;
              bit_in_byte = 0;
            }
          }
        }
        // Flush the partial byte only at the very end of the glyph bitmap
        if (bit_in_byte > 0) {
          out_byte <<= (8 - bit_in_byte);
          compiledBitmap.push_back(out_byte);
        }

        writeBytes(outFile, compiledBitmap.data(), compiledBitmap.size());
        current_bitmap_offset += compiledBitmap.size();
        glyphs[current_glyph_idx].dataLength = compiledBitmap.size();
      } else {
        glyphs[current_glyph_idx].dataLength = 0;
      }
      current_glyph_idx++;
    }
  }

  // --- PASS 3: Rewind and fill blank tables ---
  outFile.seek(glyphsTableOffset);
  writeBytes(outFile, glyphs.data(), sizeof(EpdGlyph) * glyphs.size());

  outFile.seek(bitmapLenOffset);
  writeU32(outFile, (uint32_t)current_bitmap_offset);

  outFile.close();
  heap_caps_free(ttfBuffer);
  LOG_INF("RFC", "Streaming generator finished!");
  return true;
}

#endif
