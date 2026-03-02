#include "EpdStreamFont.h"

#ifdef ENABLE_CUSTOM_FONTS

#include <Logging.h>

#include <cstring>

#include "EpdFontSerializer.h"
#include "esp_heap_caps.h"


// ─── inline helpers ──────────────────────────────────────────────────────────

static bool readU8(EspFsFile& f, uint8_t& v) { return f.read(&v, 1) == 1; }
static bool readU16(EspFsFile& f, uint16_t& v) { return f.read(reinterpret_cast<uint8_t*>(&v), 2) == 2; }
static bool readU32(EspFsFile& f, uint32_t& v) { return f.read(reinterpret_cast<uint8_t*>(&v), 4) == 4; }
static bool readI32(EspFsFile& f, int32_t& v) { return f.read(reinterpret_cast<uint8_t*>(&v), 4) == 4; }
static bool readBytes(EspFsFile& f, void* buf, size_t n) {
  return f.read(reinterpret_cast<uint8_t*>(buf), n) == (int)n;
}

constexpr uint32_t MAGIC = 0x46445045;  // 'EPDF'

// ─── loader ──────────────────────────────────────────────────────────────────

EpdStreamFont* EpdStreamFont::load(const char* path, size_t cacheSizeBytes) {
  // Use EpdFontSerializer to load ONLY metadata (metrics, intervals, glyphs)
  // EpdFontSerializer::loadFromFile has been refactored to NOT load bitmaps.
  EpdFont* baseFont = EpdFontSerializer::loadFromFile(path);
  if (!baseFont) {
    LOG_ERR("STR", "Failed to load metadata for stream font: %s", path);
    return nullptr;
  }

  // Cast away const because EpdStreamFont takes ownership of the data block
  EpdFontData* data = const_cast<EpdFontData*>(baseFont->data);

  // We wrap the base font data. The baseFont itself (the EpdFont wrapper)
  // will be deleted, but we keep the fd data.
  EpdStreamFont* streamFont = new EpdStreamFont(data, path, cacheSizeBytes);

  // We only needed the EpdFont wrapper to get the data, but baseFont
  // was allocated as a single block [EpdFont][EpdFontData][tables...].
  // Actually, EpdFontSerializer returns a pointer to the start of that block.
  // EpdStreamFont's destructor will free it via allocatedData_ (which is the same pointer).
  // So we just return the new wrapper.

  // Note: EpdFontSerializer returns the EpdFont* which is the start of the block.
  // EpdStreamFont takes that EpdFontData* (which is inside the block).
  // This is slightly dangerous if we don't manage the pointer correctly.

  return streamFont;
}

// ─── destructor ──────────────────────────────────────────────────────────────

EpdStreamFont::EpdStreamFont(EpdFontData* data, const char* path, size_t cacheSize)
    : EpdFont(data),
      allocatedData_(data),
      path_(path),
      maxCacheSize_(cacheSize),
      currentCacheSize_(0),
      accessTick_(0) {}

EpdStreamFont::~EpdStreamFont() {
  if (allocatedData_) {
    if (allocatedData_->intervals) heap_caps_free((void*)allocatedData_->intervals);
    if (allocatedData_->glyph) heap_caps_free((void*)allocatedData_->glyph);
    delete allocatedData_;
  }
}

// ─── loader & cache methods ──────────────────────────────────────────────────

void EpdStreamFont::evictOldest() const {
  if (cache_.empty()) return;

  auto oldestIt = cache_.begin();
  for (auto it = cache_.begin(); it != cache_.end(); ++it) {
    if (it->lastAccessTick < oldestIt->lastAccessTick) {
      oldestIt = it;
    }
  }

  currentCacheSize_ -= oldestIt->buffer.capacity();
  cache_.erase(oldestIt);
}

const uint8_t* EpdStreamFont::getBitmap(uint32_t cp) const {
  const EpdGlyph* g = getGlyph(cp);
  if (!g || g->dataLength == 0) return nullptr;

  accessTick_++;

  // 1. Check LRU Cache
  for (auto& node : cache_) {
    if (node.cp == cp) {
      node.lastAccessTick = accessTick_;
      return node.buffer.data();
    }
  }

  // 2. Cache Miss - open file locally, seek to glyph data, read it, then close immediately.
  // This avoids persistent file descriptors that exhaust the OS limit between chapter loads.
  EspFsFile file;
  if (!Storage.openFileForRead("STR", path_.c_str(), file)) return nullptr;

  // Header = 15 bytes (MAGIC(4), VERSION(1), advanceY(1), ascent(4), descent(4), is2Bit(1))
  // Intervals = 4 + (intervalCount * 12 bytes each)
  uint32_t intervalBlock = 4 + (allocatedData_->intervalCount * 12);
  uint32_t baseOffset = 15 + intervalBlock;

  // Fast-forward file to read the exact static offset of the bitmap block.
  file.seek(baseOffset);
  uint32_t glyphCount;
  readU32(file, glyphCount);
  uint32_t curPos = baseOffset + 4 + (glyphCount * sizeof(EpdGlyph));
  file.seek(curPos);

  // Skip kerning headers
  uint16_t klCount, krCount;
  uint8_t klClasses, krClasses;
  readU16(file, klCount);
  readU16(file, krCount);
  readU8(file, klClasses);
  readU8(file, krClasses);

  curPos += 6 + (klCount * sizeof(EpdKernClassEntry)) + (krCount * sizeof(EpdKernClassEntry)) + (klClasses * krClasses);
  file.seek(curPos);

  uint32_t ligatureCount;
  readU32(file, ligatureCount);
  curPos += 4 + (ligatureCount * sizeof(EpdLigaturePair));
  file.seek(curPos);

  uint32_t bitmapTableSize;
  readU32(file, bitmapTableSize);

  uint32_t absoluteBitmapStart = file.position();

  // 3. Perform read into Cache Node
  while (currentCacheSize_ + g->dataLength > maxCacheSize_ && !cache_.empty()) {
    evictOldest();
  }

  CacheNode newNode;
  newNode.cp = cp;
  newNode.lastAccessTick = accessTick_;
  newNode.buffer.resize(g->dataLength);

  file.seek(absoluteBitmapStart + g->dataOffset);
  file.read(newNode.buffer.data(), g->dataLength);
  file.close();  // Release FD immediately so other components can open files

  currentCacheSize_ += newNode.buffer.capacity();
  cache_.push_back(std::move(newNode));

  return cache_.back().buffer.data();
}

#endif
