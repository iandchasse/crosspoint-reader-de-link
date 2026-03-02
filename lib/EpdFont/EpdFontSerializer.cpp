#include "EpdFontSerializer.h"

#ifdef ENABLE_CUSTOM_FONTS

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "esp_heap_caps.h"

// ─── helpers ──────────────────────────────────────────────────────────────────

static bool writeU8(EspFsFile& f, uint8_t v) { return f.write(&v, 1) == 1; }
static bool writeU16(EspFsFile& f, uint16_t v) { return f.write(reinterpret_cast<const uint8_t*>(&v), 2) == 2; }
static bool writeU32(EspFsFile& f, uint32_t v) { return f.write(reinterpret_cast<const uint8_t*>(&v), 4) == 4; }
static bool writeI32(EspFsFile& f, int32_t v) { return f.write(reinterpret_cast<const uint8_t*>(&v), 4) == 4; }
static bool writeBytes(EspFsFile& f, const void* buf, size_t n) {
  return f.write(reinterpret_cast<const uint8_t*>(buf), n) == (int)n;
}

static bool readU8(EspFsFile& f, uint8_t& v) { return f.read(&v, 1) == 1; }
static bool readU16(EspFsFile& f, uint16_t& v) { return f.read(reinterpret_cast<uint8_t*>(&v), 2) == 2; }
static bool readU32(EspFsFile& f, uint32_t& v) { return f.read(reinterpret_cast<uint8_t*>(&v), 4) == 4; }
static bool readI32(EspFsFile& f, int32_t& v) { return f.read(reinterpret_cast<uint8_t*>(&v), 4) == 4; }
static bool readBytes(EspFsFile& f, void* buf, size_t n) {
  return f.read(reinterpret_cast<uint8_t*>(buf), n) == (int)n;
}

// ─── serialize ────────────────────────────────────────────────────────────────

bool EpdFontSerializer::serialize(const EpdFont* font, size_t glyphCount, EspFsFile& file) {
  const EpdFontData* d = font->data;
  bool ok = true;

  // Header
  ok &= writeU32(file, MAGIC);
  ok &= writeU8(file, VERSION);

  // Metrics
  ok &= writeU8(file, d->advanceY);
  ok &= writeI32(file, (int32_t)d->ascender);
  ok &= writeI32(file, (int32_t)d->descender);
  ok &= writeU8(file, d->is2Bit ? 1 : 0);

  // Intervals
  ok &= writeU32(file, (uint32_t)d->intervalCount);
  ok &= writeBytes(file, d->intervals, sizeof(EpdUnicodeInterval) * d->intervalCount);

  // Glyphs
  ok &= writeU32(file, (uint32_t)glyphCount);
  ok &= writeBytes(file, d->glyph, sizeof(EpdGlyph) * glyphCount);

  // Kerning
  ok &= writeU16(file, d->kernLeftEntryCount);
  ok &= writeU16(file, d->kernRightEntryCount);
  ok &= writeU8(file, d->kernLeftClassCount);
  ok &= writeU8(file, d->kernRightClassCount);
  if (d->kernLeftEntryCount > 0)
    ok &= writeBytes(file, d->kernLeftClasses, sizeof(EpdKernClassEntry) * d->kernLeftEntryCount);
  if (d->kernRightEntryCount > 0)
    ok &= writeBytes(file, d->kernRightClasses, sizeof(EpdKernClassEntry) * d->kernRightEntryCount);
  size_t matrixSize = (size_t)d->kernLeftClassCount * d->kernRightClassCount;
  if (matrixSize > 0) ok &= writeBytes(file, d->kernMatrix, matrixSize);

  // Ligatures
  ok &= writeU32(file, (uint32_t)d->ligaturePairCount);
  if (d->ligaturePairCount > 0)
    ok &= writeBytes(file, d->ligaturePairs, sizeof(EpdLigaturePair) * d->ligaturePairCount);

  // Bitmap
  // Compute bitmap size from last glyph's offset + dataLength
  uint32_t bitmapSize = 0;
  if (glyphCount > 0) {
    const EpdGlyph& last = d->glyph[glyphCount - 1];
    bitmapSize = last.dataOffset + last.dataLength;
  }
  ok &= writeU32(file, bitmapSize);
  if (bitmapSize > 0) ok &= writeBytes(file, d->bitmap, bitmapSize);

  if (!ok) LOG_ERR("SER", "Error writing .epdfont file");
  return ok;
}

// ─── loadFromFile ─────────────────────────────────────────────────────────────

EpdFont* EpdFontSerializer::loadFromFile(const char* path) {
  EspFsFile file;
  if (!Storage.openFileForRead("SER", path, file)) {
    LOG_ERR("SER", "Cannot open: %s", path);
    return nullptr;
  }

  // Header
  uint32_t magic = 0;
  uint8_t version = 0;
  if (!readU32(file, magic) || magic != MAGIC) {
    LOG_ERR("SER", "Bad magic in %s", path);
    file.close();
    return nullptr;
  }
  if (!readU8(file, version) || version != VERSION) {
    LOG_ERR("SER", "Unknown version %d in %s", version, path);
    file.close();
    return nullptr;
  }

  // Metrics
  uint8_t advY = 0;
  int32_t asc = 0, desc = 0;
  uint8_t is2b = 0;
  if (!readU8(file, advY) || !readI32(file, asc) || !readI32(file, desc) || !readU8(file, is2b)) goto rd_err;

  {
    // Intervals
    uint32_t intCount = 0;
    if (!readU32(file, intCount)) goto rd_err;

    // Glyphs
    uint32_t glyphCount = 0;
    // (read after intervals)

    // Kern
    uint16_t klEntries = 0, krEntries = 0;
    uint8_t klClasses = 0, krClasses = 0;

    // Ligatures
    uint32_t ligCount = 0;

    // Bitmap
    uint32_t bitmapSize = 0;

    // --- sizing pass for single-block PSRAM alloc ---
    auto align4 = [](size_t s) { return (s + 3) & ~3u; };
    size_t szFont = align4(sizeof(EpdFont));
    size_t szFontData = align4(sizeof(EpdFontData));

    // Read ahead to get counts (we'll re-read data below)
    // Strategy: read all variable-length data into temporary heap buffers,
    // then calculate total size, alloc one PSRAM block, copy in.

    // Intervals temp
    size_t intBytes = sizeof(EpdUnicodeInterval) * intCount;
    uint8_t* tmpIntervals = intCount ? (uint8_t*)malloc(intBytes) : nullptr;
    if (intCount && (!tmpIntervals || !readBytes(file, tmpIntervals, intBytes))) goto rd_err;

    if (!readU32(file, glyphCount)) goto rd_err;
    size_t glyphBytes = sizeof(EpdGlyph) * glyphCount;
    uint8_t* tmpGlyphs = glyphCount ? (uint8_t*)malloc(glyphBytes) : nullptr;
    if (glyphCount && (!tmpGlyphs || !readBytes(file, tmpGlyphs, glyphBytes))) {
      free(tmpIntervals);
      goto rd_err;
    }

    if (!readU16(file, klEntries) || !readU16(file, krEntries) || !readU8(file, klClasses) ||
        !readU8(file, krClasses)) {
      free(tmpIntervals);
      free(tmpGlyphs);
      goto rd_err;
    }
    size_t kernLBytes = sizeof(EpdKernClassEntry) * klEntries;
    size_t kernRBytes = sizeof(EpdKernClassEntry) * krEntries;
    size_t matrixBytes = (size_t)klClasses * krClasses;

    uint8_t* tmpKernL = klEntries ? (uint8_t*)malloc(kernLBytes) : nullptr;
    uint8_t* tmpKernR = krEntries ? (uint8_t*)malloc(kernRBytes) : nullptr;
    uint8_t* tmpMatrix = matrixBytes ? (uint8_t*)malloc(matrixBytes) : nullptr;

    if ((klEntries && (!tmpKernL || !readBytes(file, tmpKernL, kernLBytes))) ||
        (krEntries && (!tmpKernR || !readBytes(file, tmpKernR, kernRBytes))) ||
        (matrixBytes && (!tmpMatrix || !readBytes(file, tmpMatrix, matrixBytes)))) {
      free(tmpIntervals);
      free(tmpGlyphs);
      free(tmpKernL);
      free(tmpKernR);
      free(tmpMatrix);
      goto rd_err;
    }

    if (!readU32(file, ligCount)) {
      free(tmpIntervals);
      free(tmpGlyphs);
      free(tmpKernL);
      free(tmpKernR);
      free(tmpMatrix);
      goto rd_err;
    }
    size_t ligBytes = sizeof(EpdLigaturePair) * ligCount;
    uint8_t* tmpLig = ligCount ? (uint8_t*)malloc(ligBytes) : nullptr;
    if (ligCount && (!tmpLig || !readBytes(file, tmpLig, ligBytes))) {
      free(tmpIntervals);
      free(tmpGlyphs);
      free(tmpKernL);
      free(tmpKernR);
      free(tmpMatrix);
      free(tmpLig);
      goto rd_err;
    }

    if (!readU32(file, bitmapSize)) {
      free(tmpIntervals);
      free(tmpGlyphs);
      free(tmpKernL);
      free(tmpKernR);
      free(tmpMatrix);
      free(tmpLig);
      goto rd_err;
    }

    file.close();  // done with SD

    // --- metadata-only allocation in internal SRAM (No PSRAM) ---
    // We NO LONGER allocate space for or load the bitmap here.
    // This function now returns an EpdFont with EpdFontData->bitmap = nullptr,
    // which signifies to the renderer that it's a streaming/incomplete font.
    size_t total = szFont + szFontData + align4(intBytes) + align4(glyphBytes) + align4(kernLBytes) +
                   align4(kernRBytes) + align4(matrixBytes) + align4(ligBytes);

    uint8_t* mem = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!mem) {
      LOG_ERR("SER", "Failed to allocate metadata %zu bytes in internal RAM for %s", total, path);
      free(tmpIntervals);
      free(tmpGlyphs);
      free(tmpKernL);
      free(tmpKernR);
      free(tmpMatrix);
      free(tmpLig);
      return nullptr;
    }
    memset(mem, 0, total);

    uint8_t* p = mem;
    EpdFont* epdFont = (EpdFont*)p;
    p += szFont;
    EpdFontData* fd = (EpdFontData*)p;
    p += szFontData;

    auto copyBlock = [&](uint8_t* src, size_t len) -> uint8_t* {
      if (!src || len == 0) return nullptr;
      uint8_t* dst = p;
      memcpy(dst, src, len);
      p += align4(len);
      return dst;
    };

    auto* pIntervals = (EpdUnicodeInterval*)copyBlock(tmpIntervals, intBytes);
    auto* pGlyphs = (EpdGlyph*)copyBlock(tmpGlyphs, glyphBytes);
    auto* pKernL = (EpdKernClassEntry*)copyBlock(tmpKernL, kernLBytes);
    auto* pKernR = (EpdKernClassEntry*)copyBlock(tmpKernR, krEntries ? kernRBytes : 0);
    auto* pMatrix = (int8_t*)copyBlock(tmpMatrix, matrixBytes);
    auto* pLig = (EpdLigaturePair*)copyBlock(tmpLig, ligBytes);

    // Free temp buffers
    free(tmpIntervals);
    free(tmpGlyphs);
    free(tmpKernL);
    free(tmpKernR);
    free(tmpMatrix);
    free(tmpLig);

    // Wire up the structs
    new (epdFont) EpdFont(fd);
    fd->bitmap = nullptr;  ///< BITMAP IS NOT LOADED - MUST BE STREAMED
    fd->glyph = pGlyphs;
    fd->intervals = pIntervals;
    fd->intervalCount = intCount;
    fd->advanceY = advY;
    fd->ascender = asc;
    fd->descender = desc;
    fd->is2Bit = is2b != 0;
    fd->groups = nullptr;
    fd->groupCount = 0;
    fd->kernLeftClasses = pKernL;
    fd->kernRightClasses = pKernR;
    fd->kernMatrix = pMatrix;
    fd->kernLeftEntryCount = klEntries;
    fd->kernRightEntryCount = krEntries;
    fd->kernLeftClassCount = klClasses;
    fd->kernRightClassCount = krClasses;
    fd->ligaturePairs = pLig;
    fd->ligaturePairCount = ligCount;
    fd->totalAllocatedSize = total;

    LOG_INF("SER", "Loaded metadata for %s (%zu bytes internal RAM, %u glyphs)", path, total, glyphCount);
    return epdFont;
  }

rd_err:
  LOG_ERR("SER", "Read error in %s", path);
  file.close();
  return nullptr;
}

// ─── freeFont ─────────────────────────────────────────────────────────────────

void EpdFontSerializer::freeFont(EpdFont* font) {
  if (font) heap_caps_free(font);
}

#endif  // ENABLE_CUSTOM_FONTS
