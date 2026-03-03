#include "RuntimeFontConverter.h"

#ifdef ENABLE_CUSTOM_FONTS

// stb_truetype declarations come in via TtfTableLoader.h (implementation is in TtfTableLoader.cpp)
#include <FS.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cmath>
#include <vector>

#include "EpdFontSerializer.h"
#include "EpdStreamFont.h"
#include "TtfTableLoader.h"
#include "esp_heap_caps.h"
#include "stb_truetype.h"


struct EpdUnicodeIntervalBase {
  uint32_t first;
  uint32_t last;
};

// Based on fontconvert.py basic ranges
static const EpdUnicodeIntervalBase baseIntervals[] = {
    {0x0000, 0x007F}, {0x0080, 0x00FF}, {0x0100, 0x017F}, {0x01A0, 0x01A1}, {0x01AF, 0x01B0},
    {0x01C4, 0x021F}, {0x0300, 0x036F}, {0x0400, 0x04FF}, {0x1EA0, 0x1EF9}, {0x2000, 0x20CF},
    {0x2190, 0x21FF}, {0x2200, 0x22FF}, {0xFB00, 0xFB06}, {0xFFFD, 0xFFFD}};

// ─── generateEpdFontFromPath ──────────────────────────────────────────────────
// Save-then-stream: convert a TTF on the SD card to an .epdfont file, then
// return an EpdStreamFont that lazily reads bitmaps from that file.

EpdFont* RuntimeFontConverter::generateEpdFontFromPath(const char* sdPath, int sizePt, bool is2Bit,
                                                       const char* charsets) {
  char tmpEpdPath[256];
  snprintf(tmpEpdPath, sizeof(tmpEpdPath), "/tmp/runtime_font_%d.epdfont", sizePt);

  if (!generateAndSaveToFile(sdPath, sizePt, is2Bit, tmpEpdPath, charsets)) {
    LOG_ERR("RFC", "Failed to generate font to %s", tmpEpdPath);
    return nullptr;
  }

  return EpdStreamFont::load(tmpEpdPath);
}

// ─── freeEpdFont ─────────────────────────────────────────────────────────────

void RuntimeFontConverter::freeEpdFont(EpdFont* font) {
  if (!font) return;
  heap_caps_free(font);
}

// ─── countGlyphs ─────────────────────────────────────────────────────────────

size_t RuntimeFontConverter::countGlyphs(const EpdFont* font) {
  if (!font || !font->data) return 0;
  size_t total = 0;
  for (uint32_t i = 0; i < font->data->intervalCount; ++i) {
    const EpdUnicodeInterval& iv = font->data->intervals[i];
    total += (iv.last - iv.first + 1);
  }
  return total;
}

// ─── Write helpers ────────────────────────────────────────────────────────────

static bool writeU8(EspFsFile& f, uint8_t v) { return f.write(&v, 1) == 1; }
static bool writeU16(EspFsFile& f, uint16_t v) { return f.write(reinterpret_cast<const uint8_t*>(&v), 2) == 2; }
static bool writeU32(EspFsFile& f, uint32_t v) { return f.write(reinterpret_cast<const uint8_t*>(&v), 4) == 4; }
static bool writeI32(EspFsFile& f, int32_t v) { return f.write(reinterpret_cast<const uint8_t*>(&v), 4) == 4; }
static bool writeBytes(EspFsFile& f, const void* buf, size_t n) {
  return f.write(reinterpret_cast<const uint8_t*>(buf), n) == (int)n;
}

// ─── UTF-8 decoder helper ────────────────────────────────────────────────────

static void parseCharsets(const char* charsets, std::vector<uint32_t>& out) {
  if (!charsets || charsets[0] == '\0') return;
  const uint8_t* p = (const uint8_t*)charsets;
  while (*p) {
    uint32_t c = *p;
    if (c < 0x80) {
      out.push_back(c);
      p++;
    } else if ((c & 0xE0) == 0xC0) {
      out.push_back(((c & 0x1F) << 6) | (p[1] & 0x3F));
      p += 2;
    } else if ((c & 0xF0) == 0xE0) {
      out.push_back(((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
      p += 3;
    } else if ((c & 0xF8) == 0xF0) {
      out.push_back(((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F));
      p += 4;
    } else {
      p++;
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
}

static bool wantCodepoint(const std::vector<uint32_t>& subset, uint32_t cp) {
  if (subset.empty()) return true;
  return std::binary_search(subset.begin(), subset.end(), cp);
}

// ─── generateAndSaveToFile ────────────────────────────────────────────────────
//
// Two-pass streaming font generation using TtfTableLoader for windowed glyph
// access. Peak RAM is ~80 KB regardless of TTF file size.
//
// Pass 1: Count valid glyphs and build unicode interval list (no rasterization).
// Pass 2: Rasterize and write bitmaps directly to the output .epdfont file.

bool RuntimeFontConverter::generateAndSaveToFile(const char* sdTtfPath, int sizePt, bool is2Bit,
                                                 const char* outEpdFontPath, const char* charsets) {
  LOG_INF("RFC", "Streaming font gen: %s -> %s (pt=%d, 2bit=%d)", sdTtfPath, outEpdFontPath, sizePt, is2Bit);
  LOG_INF("RFC", "Free heap before TTF load: %u bytes", ESP.getFreeHeap());

  // ─── Open TTF with windowed loader (no full-file alloc) ───────────────────
  TtfTableLoader loader;
  if (!loader.open(sdTtfPath)) {
    LOG_ERR("RFC", "TtfTableLoader failed to open %s", sdTtfPath);
    return false;
  }

  stbtt_fontinfo fontInfo = *loader.fontInfo();

  LOG_INF("RFC", "Free heap after TTF loader init: %u bytes", ESP.getFreeHeap());

  // ─── Compute font metrics ─────────────────────────────────────────────────
  float ppem = sizePt * 150.0f / 72.0f;
  float scale = stbtt_ScaleForMappingEmToPixels(&fontInfo, ppem);

  int ascent_units, descent_units, lineGap_units;
  stbtt_GetFontVMetrics(&fontInfo, &ascent_units, &descent_units, &lineGap_units);
  int ascent = (int)roundf(ascent_units * scale);
  int descent = (int)roundf(descent_units * scale);
  int advanceY = ascent - descent + (int)roundf(lineGap_units * scale);

  std::vector<uint32_t> customCodepoints;
  parseCharsets(charsets, customCodepoints);

  // ─── PASS 1: Count glyphs and build interval list ─────────────────────────
  // NOTE: stbtt_FindGlyphIndex and stbtt_GetGlyphBitmapBox don't touch glyf,
  // so we do NOT need to call loader.loadGlyph() in this pass.
  std::vector<EpdUnicodeInterval> intervals;
  size_t valid_glyph_count = 0;

  for (size_t ri = 0; ri < sizeof(baseIntervals) / sizeof(baseIntervals[0]); ++ri) {
    bool in_range = false;
    uint32_t current_first = 0;
    uint32_t current_offset = valid_glyph_count;

    for (uint32_t cp = baseIntervals[ri].first; cp <= baseIntervals[ri].last; ++cp) {
      if (!wantCodepoint(customCodepoints, cp)) {
        if (in_range) {
          intervals.push_back({current_first, cp - 1, current_offset});
          in_range = false;
        }
        continue;
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
      intervals.push_back({current_first, baseIntervals[ri].last, current_offset});
    }
  }

  if (valid_glyph_count == 0) {
    LOG_ERR("RFC", "No valid glyphs found in %s", sdTtfPath);
    loader.close();
    return false;
  }

  LOG_INF("RFC", "Pass 1 done: %zu glyphs, %zu intervals", valid_glyph_count, intervals.size());

  // ─── Open output file ─────────────────────────────────────────────────────
  {
    String outPathStr = outEpdFontPath;
    int lastSlash = outPathStr.lastIndexOf('/');
    if (lastSlash > 0) {
      String parentDir = outPathStr.substring(0, lastSlash);
      if (!Storage.exists(parentDir.c_str())) Storage.mkdir(parentDir.c_str());
    }
  }

  EspFsFile outFile;
  if (!Storage.openFileForWrite("RFC", outEpdFontPath, outFile)) {
    LOG_ERR("RFC", "Cannot open output file: %s", outEpdFontPath);
    loader.close();
    return false;
  }

  // ─── Write file header ────────────────────────────────────────────────────
  writeU32(outFile, EpdFontSerializer::MAGIC);
  writeU8(outFile, EpdFontSerializer::VERSION);
  writeU8(outFile, (uint8_t)std::min(255, std::max(0, advanceY)));
  writeI32(outFile, (int32_t)ascent);
  writeI32(outFile, (int32_t)descent);
  writeU8(outFile, is2Bit ? 1 : 0);

  writeU32(outFile, (uint32_t)intervals.size());
  writeBytes(outFile, intervals.data(), sizeof(EpdUnicodeInterval) * intervals.size());

  writeU32(outFile, (uint32_t)valid_glyph_count);
  // Reserve space for glyph table (filled in pass 3)
  uint32_t glyphsTableOffset = outFile.position();
  for (size_t i = 0; i < valid_glyph_count * sizeof(EpdGlyph); i++) writeU8(outFile, 0);

  // Empty kern/ligature tables
  writeU16(outFile, 0);
  writeU16(outFile, 0);
  writeU8(outFile, 0);
  writeU8(outFile, 0);
  writeU32(outFile, 0);

  // Placeholder for bitmap size
  uint32_t bitmapLenOffset = outFile.position();
  writeU32(outFile, 0);

  // ─── PASS 2: Rasterize and write bitmaps ──────────────────────────────────
  // loadGlyph() is called here before each stbtt_MakeGlyphBitmap call.
  std::vector<EpdGlyph> glyphs(valid_glyph_count);
  size_t current_glyph_idx = 0;
  size_t current_bitmap_offset = 0;

  for (size_t ri = 0; ri < intervals.size(); ++ri) {
    for (uint32_t cp = intervals[ri].first; cp <= intervals[ri].last; ++cp) {
      if (!wantCodepoint(customCodepoints, cp)) continue;

      int glyphIndex = stbtt_FindGlyphIndex(&fontInfo, cp);
      if (glyphIndex <= 0) continue;

      // *** Load this glyph's outline data from SD into the scratch zone ***
      if (!loader.loadGlyph(glyphIndex)) {
        LOG_ERR("RFC", "loadGlyph failed for cp=0x%04X (idx=%d)", cp, glyphIndex);
        outFile.close();
        loader.close();
        Storage.remove(outEpdFontPath);
        return false;
      }

      int x0, y0, x1, y1;
      int advanceWidth, leftSideBearing;
      stbtt_GetGlyphHMetrics(&fontInfo, glyphIndex, &advanceWidth, &leftSideBearing);
      stbtt_GetGlyphBitmapBox(&fontInfo, glyphIndex, scale, scale, &x0, &y0, &x1, &y1);

      int w = x1 - x0;
      int h = y1 - y0;
      int advanceX_fp4 = (int)roundf(advanceWidth * scale * 16.0f);

      EpdGlyph& g = glyphs[current_glyph_idx];
      g.width = (uint8_t)w;
      g.height = (uint8_t)h;
      g.advanceX = (uint16_t)std::min(65535, std::max(0, advanceX_fp4));
      g.left = (int16_t)x0;
      g.top = (int16_t)(-y0);
      g.dataOffset = (uint32_t)current_bitmap_offset;

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
        if (bit_in_byte > 0) {
          out_byte <<= (8 - bit_in_byte);
          compiledBitmap.push_back(out_byte);
        }

        writeBytes(outFile, compiledBitmap.data(), compiledBitmap.size());
        current_bitmap_offset += compiledBitmap.size();
        g.dataLength = (uint16_t)compiledBitmap.size();
      } else {
        g.dataLength = 0;
      }

      current_glyph_idx++;
    }
  }

  // ─── PASS 3: Rewind and write glyph table + bitmap size ──────────────────
  outFile.seek(glyphsTableOffset);
  writeBytes(outFile, glyphs.data(), sizeof(EpdGlyph) * glyphs.size());

  outFile.seek(bitmapLenOffset);
  writeU32(outFile, (uint32_t)current_bitmap_offset);

  outFile.close();
  loader.close();

  LOG_INF("RFC", "Font saved: %zu glyphs, %zu bitmap bytes -> %s", valid_glyph_count, current_bitmap_offset,
          outEpdFontPath);
  LOG_INF("RFC", "Free heap after generation: %u bytes", ESP.getFreeHeap());
  return true;
}

#endif  // ENABLE_CUSTOM_FONTS
