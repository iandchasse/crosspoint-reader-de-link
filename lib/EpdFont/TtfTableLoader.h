#pragma once

#ifdef ENABLE_CUSTOM_FONTS

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

// Including stb_truetype.h WITHOUT STB_TRUETYPE_IMPLEMENTATION gives only
// type definitions (stbtt_fontinfo etc.) and function declarations.
// The implementation is compiled exactly once via TtfTableLoader.cpp which
// #defines STB_TRUETYPE_IMPLEMENTATION before #including this header.
#include "stb_truetype.h"

/**
 * @brief Windowed TTF table loader for low-RAM devices (no PSRAM).
 *
 * Builds an ~80 KB synthetic TTF buffer containing only metric tables
 * (head, hhea, maxp, cmap, hmtx, loca) plus a 16 KB scratch zone.
 * Glyph outline data (glyf) is lazily seeked from the SD file into
 * the scratch zone one glyph at a time before each stb_truetype call.
 *
 * Usage:
 *   TtfTableLoader loader;
 *   if (!loader.open(path)) return false;
 *   stbtt_fontinfo* info = loader.fontInfo();
 *   // ...
 *   for each glyph index:
 *     loader.loadGlyph(glyphIndex);
 *     stbtt_MakeGlyphBitmap(info, ...);
 *   loader.close();
 */
class TtfTableLoader {
 public:
  TtfTableLoader();
  ~TtfTableLoader();

  // Non-copyable
  TtfTableLoader(const TtfTableLoader&) = delete;
  TtfTableLoader& operator=(const TtfTableLoader&) = delete;

  /**
   * @brief Open a TTF file and build the synthetic buffer.
   * @param path Full path to the .ttf file on storage.
   * @return true on success; false on I/O error, bad format, or OOM.
   */
  bool open(const char* path);

  /**
   * @brief Load one glyph's outline data into the scratch zone.
   *        Patches synthetic loca so stb_truetype finds the data.
   *        Handles composite glyphs by recursively loading components.
   * @param glyphIndex stb_truetype glyph index (from stbtt_FindGlyphIndex).
   * @return true if the glyph was successfully loaded (or is empty).
   */
  bool loadGlyph(int glyphIndex);

  /**
   * @brief Returns pointer to synthetic buffer, suitable for stbtt_InitFont(..., 0).
   */
  const uint8_t* synthBuffer() const { return synthBuf_; }

  /**
   * @brief Returns initialized stbtt_fontinfo. Valid after open() succeeds.
   */
  stbtt_fontinfo* fontInfo() { return &fontInfo_; }

  /**
   * @brief Close the SD file and release all resources.
   */
  void close();

 private:
  // ─── Constants ──────────────────────────────────────────────────────────────
  static constexpr size_t SCRATCH_SIZE = 16 * 1024;      ///< Scratch zone for one glyph's outline data
  static constexpr size_t HEADER_RESERVE = 12 + 7 * 16;  ///< Offset table + 7 directory entries
  static constexpr size_t MAX_METRIC_SIZE = 60 * 1024;   ///< Safety cap for metric tables (cmap can be large)

  // ─── TTF table tags (big-endian) ────────────────────────────────────────────
  static constexpr uint32_t TAG_head = 0x68656164u;
  static constexpr uint32_t TAG_hhea = 0x68686561u;
  static constexpr uint32_t TAG_maxp = 0x6D617870u;
  static constexpr uint32_t TAG_cmap = 0x636D6170u;
  static constexpr uint32_t TAG_hmtx = 0x686D7478u;
  static constexpr uint32_t TAG_loca = 0x6C6F6361u;
  static constexpr uint32_t TAG_glyf = 0x676C7966u;

  // ─── Helpers ────────────────────────────────────────────────────────────────
  struct TableRecord {
    uint32_t tag;
    uint32_t checksum;
    uint32_t offset;  ///< Offset in the real TTF file
    uint32_t length;
  };

  static uint16_t beU16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
  static uint32_t beU32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
  }
  static void putBeU16(uint8_t* p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
  }
  static void putBeU32(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
  }

  bool readTableDirectory();
  bool loadMetricTables();
  bool buildSynthHeader();
  bool parseLoca();
  bool loadGlyphInternal(int glyphIndex, uint8_t* dest, size_t& bytesUsed);
  void patchSynthLoca(int idx, uint32_t scratchByteOffset, uint32_t glyphDataLen);

  // ─── State ──────────────────────────────────────────────────────────────────
  EspFsFile file_;

  uint8_t* synthBuf_ = nullptr;     ///< The synthetic buffer (sized to actual metric tables + scratch)
  size_t synthBufSize_ = 0;         ///< Actual allocation size (computed per-TTF file)
  size_t synthBufUsed_ = 0;         ///< Bytes used so far (before scratch zone)
  uint8_t* scratchZone_ = nullptr;  ///< Pointer into synthBuf_ where scratch starts
  size_t scratchOffset_ = 0;        ///< Offset of scratch zone within synthBuf_

  stbtt_fontinfo fontInfo_;

  // Table records from the real TTF file
  TableRecord tables_[7];
  int tableCount_ = 0;

  // Location of each loaded table within synthBuf_
  uint32_t synthOffsets_[7] = {};  ///< Where each table was placed in synthBuf_

  // Position in synthBuf_ where the synthetic table directory entries are written
  // so we can patch glyf offset later
  uint8_t* synthDirBase_ = nullptr;  ///< Points to first 16-byte directory entry
  int glyfDirIdx_ = -1;              ///< Which directory index is "glyf"
  int locaDirIdx_ = -1;              ///< Which directory index is "loca"

  // Parsed loca table
  uint32_t* locaOffsets_ = nullptr;  ///< Real file offsets per glyph
  int numGlyphs_ = 0;
  bool locaIsLong_ = false;
  uint32_t glyfFileOffset_ = 0;  ///< Real file offset of glyf table

  // Synthetic loca (points into synthBuf_, patched per glyph)
  uint8_t* synthLoca_ = nullptr;  ///< Pointer to loca data within synthBuf_
  bool synthLocaIsLong_ = false;
  int synthNumGlyphs_ = 0;
};

#endif  // ENABLE_CUSTOM_FONTS
