// STB_TRUETYPE_IMPLEMENTATION must be defined before ANY include of stb_truetype.h.
// TtfTableLoader.h includes stb_truetype.h, so this define MUST be first.
#define STB_TRUETYPE_IMPLEMENTATION
#include "TtfTableLoader.h"

#ifdef ENABLE_CUSTOM_FONTS

#include <Logging.h>

#include <cstring>

#include "esp_heap_caps.h"

// stb_truetype.h already included via TtfTableLoader.h above (with implementation).

// ─── Constructor / Destructor ─────────────────────────────────────────────────

TtfTableLoader::TtfTableLoader() { memset(&fontInfo_, 0, sizeof(fontInfo_)); }

TtfTableLoader::~TtfTableLoader() { close(); }

void TtfTableLoader::close() {
  file_.close();
  if (synthBuf_) {
    heap_caps_free(synthBuf_);
    synthBuf_ = nullptr;
  }
  if (locaOffsets_) {
    free(locaOffsets_);
    locaOffsets_ = nullptr;
  }
  scratchZone_ = nullptr;
  synthDirBase_ = nullptr;
  synthLoca_ = nullptr;
  numGlyphs_ = 0;
  tableCount_ = 0;
}

// ─── open ─────────────────────────────────────────────────────────────────────

bool TtfTableLoader::open(const char* path) {
  if (!Storage.openFileForRead("TTL", path, file_)) {
    LOG_ERR("TTL", "Cannot open TTF: %s", path);
    return false;
  }

  if (!readTableDirectory()) {
    LOG_ERR("TTL", "Failed to parse TTF table directory in %s", path);
    file_.close();
    return false;
  }

  // synthBufSize_ was set by readTableDirectory() to the raw metric table bytes.
  // Now add the header reserve and scratch zone to get the total allocation.
  size_t metricBytes = synthBufSize_;
  synthBufSize_ = HEADER_RESERVE + metricBytes + SCRATCH_SIZE;
  LOG_INF("TTL", "Allocating %zu bytes for synthetic buffer (header=%zu + metric=%zu + scratch=%zu)", synthBufSize_,
          HEADER_RESERVE, metricBytes, SCRATCH_SIZE);
  synthBuf_ = (uint8_t*)heap_caps_malloc(synthBufSize_, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!synthBuf_) {
    LOG_ERR("TTL", "Failed to allocate %zu bytes for synthetic TTF buffer", synthBufSize_);
    file_.close();
    return false;
  }
  memset(synthBuf_, 0, synthBufSize_);

  if (!loadMetricTables()) {
    LOG_ERR("TTL", "Failed to load metric tables");
    close();
    return false;
  }

  // The scratch zone starts right after the metric data
  scratchOffset_ = synthBufUsed_;
  scratchZone_ = synthBuf_ + scratchOffset_;

  if (!buildSynthHeader()) {
    LOG_ERR("TTL", "Failed to build synthetic TTF header");
    close();
    return false;
  }

  if (!parseLoca()) {
    LOG_ERR("TTL", "Failed to parse loca table");
    close();
    return false;
  }

  if (!stbtt_InitFont(&fontInfo_, synthBuf_, 0)) {
    LOG_ERR("TTL", "stbtt_InitFont failed on synthetic buffer");
    close();
    return false;
  }

  LOG_INF("TTL", "Opened TTF with windowed loader (%d glyphs, synth=%zu bytes)", numGlyphs_, synthBufUsed_);
  return true;
}

// ─── readTableDirectory ───────────────────────────────────────────────────────

bool TtfTableLoader::readTableDirectory() {
  // TTF offset table: sfVersion(4), numTables(2), searchRange(2), entrySelector(2), rangeShift(2)
  uint8_t hdr[12];
  if (file_.read(hdr, 12) != 12) return false;

  uint16_t numTables = beU16(hdr + 4);
  if (numTables > 64) {
    LOG_ERR("TTL", "Suspicious numTables=%d", numTables);
    return false;
  }

  static const uint32_t NEEDED[] = {TAG_head, TAG_hhea, TAG_maxp, TAG_cmap, TAG_hmtx, TAG_loca, TAG_glyf};
  constexpr int NUM_NEEDED = 7;

  // Sum up metric table sizes (all tables except glyf) to compute the dynamic allocation.
  // Each table is 4-byte aligned in the synth buffer.
  // synthBufSize_ at this point holds the metric-only byte count (set below);
  // HEADER_RESERVE and SCRATCH_SIZE are added in open().
  uint32_t metricBytes = 0;
  tableCount_ = 0;
  for (int i = 0; i < numTables; i++) {
    uint8_t rec[16];
    if (file_.read(rec, 16) != 16) return false;

    uint32_t tag = beU32(rec);
    for (int j = 0; j < NUM_NEEDED; j++) {
      if (tag == NEEDED[j]) {
        if (tableCount_ < 7) {
          tables_[tableCount_].tag = tag;
          tables_[tableCount_].checksum = beU32(rec + 4);
          tables_[tableCount_].offset = beU32(rec + 8);
          tables_[tableCount_].length = beU32(rec + 12);
          if (tag != TAG_glyf) {
            // Account for 4-byte alignment padding
            uint32_t aligned = (tables_[tableCount_].length + 3) & ~3u;
            metricBytes += aligned;
          }
          tableCount_++;
        }
        break;
      }
    }
  }

  // Safety cap: if cmap or hmtx are pathologically large, bail out early
  if (metricBytes > MAX_METRIC_SIZE) {
    LOG_ERR("TTL", "Metric tables too large (%u bytes, max %zu)", metricBytes, MAX_METRIC_SIZE);
    return false;
  }
  synthBufSize_ = metricBytes;  // open() will add HEADER_RESERVE + SCRATCH_SIZE

  // Verify we found glyf and loca at minimum
  bool hasGlyf = false, hasLoca = false, hasHead = false;
  for (int i = 0; i < tableCount_; i++) {
    if (tables_[i].tag == TAG_glyf) {
      hasGlyf = true;
      glyfFileOffset_ = tables_[i].offset;
    }
    if (tables_[i].tag == TAG_loca) hasLoca = true;
    if (tables_[i].tag == TAG_head) hasHead = true;
  }

  if (!hasGlyf || !hasLoca || !hasHead) {
    LOG_ERR("TTL", "Missing required TTF tables (glyf=%d loca=%d head=%d)", hasGlyf, hasLoca, hasHead);
    return false;
  }

  return true;
}

// ─── loadMetricTables ─────────────────────────────────────────────────────────

bool TtfTableLoader::loadMetricTables() {
  // Reserve the first 12 + 7*16 = 124 bytes for the synthetic header (filled later)
  constexpr size_t HEADER_RESERVE = 12 + 7 * 16;
  synthBufUsed_ = HEADER_RESERVE;
  synthDirBase_ = synthBuf_ + 12;  // Points to start of table directory

  for (int i = 0; i < tableCount_; i++) {
    if (tables_[i].tag == TAG_glyf) {
      // glyf is NOT loaded into synthBuf_ - it comes from the scratch zone.
      // Record -1 as a sentinel so we know to use scratchOffset_ later.
      glyfDirIdx_ = i;
      synthOffsets_[i] = 0;  // Will be patched in buildSynthHeader
      continue;
    }

    if (tables_[i].tag == TAG_loca) {
      locaDirIdx_ = i;
    }

    uint32_t len = tables_[i].length;
    if (synthBufUsed_ + len + SCRATCH_SIZE > synthBufSize_) {
      LOG_ERR("TTL", "Synthetic buffer overflow: need %zu, allocated %zu", synthBufUsed_ + len + SCRATCH_SIZE,
              synthBufSize_);
      return false;
    }

    // Seek and read the table into the synthetic buffer
    file_.seek(tables_[i].offset);
    if (file_.read(synthBuf_ + synthBufUsed_, len) != (int)len) {
      LOG_ERR("TTL", "Failed to read table 0x%08X (len=%u)", tables_[i].tag, len);
      return false;
    }

    synthOffsets_[i] = (uint32_t)synthBufUsed_;
    synthBufUsed_ += len;

    // Align to 4 bytes
    while (synthBufUsed_ & 3) synthBufUsed_++;
  }

  return true;
}

// ─── buildSynthHeader ─────────────────────────────────────────────────────────

bool TtfTableLoader::buildSynthHeader() {
  // The scratch zone sits right after the metric tables
  // glyf directory entry will point to scratchOffset_
  // Write the synthetic offset table + table directory at synthBuf_[0..HEADER_RESERVE-1]

  uint8_t* p = synthBuf_;

  // Offset table (12 bytes): version=0x00010000, numTables, searchRange, entrySelector, rangeShift
  int n = tableCount_;
  int searchRange = 1;
  int entrySelector = 0;
  while (searchRange * 2 <= n) {
    searchRange *= 2;
    entrySelector++;
  }
  searchRange *= 16;
  int rangeShift = n * 16 - searchRange;

  putBeU32(p, 0x00010000u);  // sfVersion (TrueType)
  putBeU16(p + 4, (uint16_t)n);
  putBeU16(p + 6, (uint16_t)searchRange);
  putBeU16(p + 8, (uint16_t)entrySelector);
  putBeU16(p + 10, (uint16_t)rangeShift);
  p += 12;

  // Write table directory entries
  for (int i = 0; i < tableCount_; i++) {
    uint32_t synthOff;
    uint32_t synthLen;

    if (i == glyfDirIdx_) {
      // glyf points to the scratch zone
      synthOff = (uint32_t)scratchOffset_;
      synthLen = SCRATCH_SIZE;
    } else {
      synthOff = synthOffsets_[i];
      synthLen = tables_[i].length;
    }

    putBeU32(p, tables_[i].tag);
    putBeU32(p + 4, tables_[i].checksum);
    putBeU32(p + 8, synthOff);
    putBeU32(p + 12, synthLen);
    p += 16;
  }

  return true;
}

// ─── parseLoca ────────────────────────────────────────────────────────────────

bool TtfTableLoader::parseLoca() {
  // Find head table in synthBuf_ to get indexToLocFormat
  int headIdx = -1;
  for (int i = 0; i < tableCount_; i++) {
    if (tables_[i].tag == TAG_head) {
      headIdx = i;
      break;
    }
  }
  if (headIdx < 0) return false;

  const uint8_t* headData = synthBuf_ + synthOffsets_[headIdx];
  // head.indexToLocFormat is at offset 50 within the head table (2 bytes, big-endian)
  int16_t locFormat = (int16_t)beU16(headData + 50);
  locaIsLong_ = (locFormat == 1);

  // Find maxp to get numGlyphs
  int maxpIdx = -1;
  for (int i = 0; i < tableCount_; i++) {
    if (tables_[i].tag == TAG_maxp) {
      maxpIdx = i;
      break;
    }
  }
  if (maxpIdx < 0) return false;

  const uint8_t* maxpData = synthBuf_ + synthOffsets_[maxpIdx];
  numGlyphs_ = (int)beU16(maxpData + 4);

  // Find and parse loca
  if (locaDirIdx_ < 0) return false;
  const uint8_t* locaData = synthBuf_ + synthOffsets_[locaDirIdx_];
  uint32_t locaLen = tables_[locaDirIdx_].length;
  (void)locaLen;

  // We store locaOffsets_ as uint32_t real-file offsets into glyf table
  locaOffsets_ = (uint32_t*)malloc(sizeof(uint32_t) * (numGlyphs_ + 1));
  if (!locaOffsets_) {
    LOG_ERR("TTL", "OOM allocating locaOffsets_ for %d glyphs", numGlyphs_);
    return false;
  }

  for (int i = 0; i <= numGlyphs_; i++) {
    if (locaIsLong_) {
      locaOffsets_[i] = beU32(locaData + i * 4);
    } else {
      locaOffsets_[i] = (uint32_t)beU16(locaData + i * 2) * 2;
    }
  }

  // Keep a pointer to the synthetic loca for patching
  synthLoca_ = const_cast<uint8_t*>(locaData);
  synthLocaIsLong_ = locaIsLong_;
  synthNumGlyphs_ = numGlyphs_;

  LOG_DBG("TTL", "Loca parsed: %d glyphs, %s format", numGlyphs_, locaIsLong_ ? "long" : "short");
  return true;
}

// ─── loadGlyph ────────────────────────────────────────────────────────────────

bool TtfTableLoader::loadGlyph(int glyphIndex) {
  if (glyphIndex < 0 || glyphIndex >= numGlyphs_) {
    // Point the scratch zone to empty by making loca[i] == loca[i+1]
    patchSynthLoca(glyphIndex, 0, 0);
    return true;
  }

  size_t bytesUsed = 0;
  bool ok = loadGlyphInternal(glyphIndex, scratchZone_, bytesUsed);

  // Always patch the primary glyph's loca to point to scratch start
  // loadGlyphInternal handles patching internally per glyph
  return ok;
}

// ─── patchSynthLoca (inline helper) ──────────────────────────────────────────

// Forward declaration for use in loadGlyphInternal
static void patchSynthLocaImpl(uint8_t* synthLoca, bool isLong, int idx, uint32_t offset, uint32_t nextOffset) {
  if (isLong) {
    uint8_t* p = synthLoca + idx * 4;
    p[0] = (offset >> 24) & 0xFF;
    p[1] = (offset >> 16) & 0xFF;
    p[2] = (offset >> 8) & 0xFF;
    p[3] = offset & 0xFF;
    // Patch idx+1 as well to define the length
    p += 4;
    p[0] = (nextOffset >> 24) & 0xFF;
    p[1] = (nextOffset >> 16) & 0xFF;
    p[2] = (nextOffset >> 8) & 0xFF;
    p[3] = nextOffset & 0xFF;
  } else {
    // Short loca: offset / 2
    uint32_t s = offset / 2;
    uint32_t sn = nextOffset / 2;
    uint8_t* p = synthLoca + idx * 2;
    p[0] = (s >> 8) & 0xFF;
    p[1] = s & 0xFF;
    p += 2;
    p[0] = (sn >> 8) & 0xFF;
    p[1] = sn & 0xFF;
  }
}

void TtfTableLoader::patchSynthLoca(int idx, uint32_t scratchByteOffset, uint32_t glyphDataLen) {
  // loca offsets are RELATIVE TO THE START OF THE glyf TABLE, not to synthBuf_.
  // The glyf directory entry points to scratchOffset_ in synthBuf_.
  // So loca[i] = (absolute synthBuf_ position) - scratchOffset_.
  uint32_t relOff = scratchByteOffset;  // already relative — see loadGlyphInternal
  uint32_t relNext = relOff + glyphDataLen;
  patchSynthLocaImpl(synthLoca_, synthLocaIsLong_, idx, relOff, relNext);
}

// ─── loadGlyphInternal ────────────────────────────────────────────────────────

bool TtfTableLoader::loadGlyphInternal(int glyphIndex, uint8_t* dest, size_t& bytesUsed) {
  if (glyphIndex < 0 || glyphIndex >= numGlyphs_) {
    // Out-of-range: patch to zero-length entry at current scratch position
    uint32_t relOff = (uint32_t)(dest - scratchZone_);
    patchSynthLocaImpl(synthLoca_, synthLocaIsLong_, glyphIndex, relOff, relOff);
    return true;
  }

  uint32_t glyphStart = locaOffsets_[glyphIndex];
  uint32_t glyphEnd = locaOffsets_[glyphIndex + 1];
  uint32_t glyphLen = glyphEnd - glyphStart;

  // Empty glyph (e.g. space character)
  if (glyphLen == 0) {
    uint32_t relOff = (uint32_t)(dest - scratchZone_);
    patchSynthLocaImpl(synthLoca_, synthLocaIsLong_, glyphIndex, relOff, relOff);
    return true;
  }

  // Safety check: make sure we don't overflow the scratch zone
  if ((dest - scratchZone_) + glyphLen > SCRATCH_SIZE) {
    LOG_ERR("TTL", "Scratch overflow for glyph %d (len=%u)", glyphIndex, glyphLen);
    return false;
  }

  // Seek and read from the real file
  uint32_t fileOffset = glyfFileOffset_ + glyphStart;
  file_.seek(fileOffset);
  if (file_.read(dest, glyphLen) != (int)glyphLen) {
    LOG_ERR("TTL", "Failed to read glyph %d from file (offset=%u, len=%u)", glyphIndex, fileOffset, glyphLen);
    return false;
  }

  // Patch synthetic loca with offset RELATIVE TO glyf table start (i.e., relative to scratchZone_).
  // stb_truetype computes:  synthBuf_[glyf_start_in_buf + loca[i]]
  // glyf_start_in_buf == scratchOffset_   (from the directory entry we wrote)
  // So:  loca[i] = (dest - synthBuf_) - scratchOffset_  =  dest - scratchZone_
  uint32_t relOff = (uint32_t)(dest - scratchZone_);
  patchSynthLocaImpl(synthLoca_, synthLocaIsLong_, glyphIndex, relOff, relOff + glyphLen);
  bytesUsed += glyphLen;
  dest += glyphLen;

  // Check if composite glyph (numberOfContours < 0 means composite, stored as big-endian int16)
  int16_t numContours = (int16_t)(((uint16_t)dest[-glyphLen] << 8) | (uint16_t)dest[-glyphLen + 1]);
  if (numContours >= 0) {
    return true;  // Simple glyph — done
  }

  // Composite glyph: parse component records
  // Structure after the 10-byte glyph header:
  //   uint16 flags
  //   uint16 glyphIndex
  //   followed by variable offset data based on flags
  //
  // NOTE: comp must be non-const so we can patch matching-point composites
  // (stb_truetype asserts on these — see stb_truetype.h line 1838).
  uint8_t* comp = dest - glyphLen + 10;  // skip 10-byte header (numContours + bbox)
  uint8_t* compEnd = (uint8_t*)dest;

  constexpr uint16_t FLAG_ARG_1_AND_2_ARE_WORDS = 0x0001;
  constexpr uint16_t FLAG_ARGS_ARE_XY_VALUES = 0x0002;  // if NOT set, args are point indices
  constexpr uint16_t FLAG_WE_HAVE_A_SCALE = 0x0008;
  constexpr uint16_t FLAG_MORE_COMPONENTS = 0x0020;
  constexpr uint16_t FLAG_WE_HAVE_AN_X_AND_Y_SCALE = 0x0040;
  constexpr uint16_t FLAG_WE_HAVE_A_TWO_BY_TWO = 0x0080;

  while (comp + 4 <= compEnd) {
    uint16_t flags = beU16(comp);
    uint16_t compIdx = beU16(comp + 2);

    // stb_truetype does not implement "matching point" composites (flag bit 1 = 0).
    // It hits an STBTT_assert(0) and crashes the firmware. Detect and patch in-place:
    // Force ARGS_ARE_XY_VALUES and zero the argument bytes so stb sees XY = (0,0).
    // This places the component at origin offset which may not be pixel-perfect but
    // avoids the crash. These glyphs are rare (some accented chars) and a wrong offset
    // is far better than a hard fault.
    if (!(flags & FLAG_ARGS_ARE_XY_VALUES)) {
      uint16_t patchedFlags = flags | FLAG_ARGS_ARE_XY_VALUES;
      comp[0] = (patchedFlags >> 8) & 0xFF;
      comp[1] = patchedFlags & 0xFF;
      // Zero out the argument bytes (2 bytes for byte args, 4 for short args)
      int argBytes = (flags & FLAG_ARG_1_AND_2_ARE_WORDS) ? 4 : 2;
      memset(comp + 4, 0, argBytes);
      LOG_DBG("TTL", "Patched matching-point composite component idx=%d", compIdx);
    }

    // Load the component glyph into the scratch zone after the current data
    size_t childBytes = 0;
    if (!loadGlyphInternal((int)compIdx, dest, childBytes)) return false;
    dest += childBytes;
    bytesUsed += childBytes;

    comp += 4;  // past flags and glyph index
    // Advance past argument data
    if (flags & FLAG_ARG_1_AND_2_ARE_WORDS)
      comp += 4;  // 2×int16
    else
      comp += 2;  // 2×int8

    // Advance past optional scale/matrix data
    if (flags & FLAG_WE_HAVE_A_TWO_BY_TWO)
      comp += 8;
    else if (flags & FLAG_WE_HAVE_AN_X_AND_Y_SCALE)
      comp += 4;
    else if (flags & FLAG_WE_HAVE_A_SCALE)
      comp += 2;

    if (!(flags & FLAG_MORE_COMPONENTS)) break;
  }

  return true;
}

#endif  // ENABLE_CUSTOM_FONTS
