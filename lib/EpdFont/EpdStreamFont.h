#pragma once
#include <cstdint>
#include <vector>

#include "EpdFont.h"
#include "HalStorage.h"

#ifdef ENABLE_CUSTOM_FONTS

/**
 * @brief SD-Streaming implementation of EpdFont.
 *
 * Rather than holding the entire multi-megabyte bitmap table in PSRAM,
 * EpdStreamFont holds only the lightweight metric tables in heap, and
 * lazily fetches `getBitmap` pixel buffers directly from the `.epdfont`
 * file on the SD card into a constrained, fixed-size LRU cache.
 */
class EpdStreamFont : public EpdFont {
 public:
  /**
   * @brief Create a streaming font.
   * @param path Full path to the .epdfont file (e.g., "/.fonts/Roboto/14_regular.epdfont")
   * @param cacheSizeBytes Maximum SRAM footprint for the LRU glyph pixel cache. Default is 16KB.
   * @return Allocated caller-owned EpdStreamFont instance, or nullptr if invalid.
   */
  static EpdStreamFont* load(const char* path, size_t cacheSizeBytes = 16 * 1024);

  ~EpdStreamFont();

  // Non-copyable/movable
  EpdStreamFont(const EpdStreamFont&) = delete;
  EpdStreamFont& operator=(const EpdStreamFont&) = delete;

  /**
   * @brief Fetch the pixel buffer for a given codepoint.
   * The returned pointer is owned by the LRU cache and is valid ONLY
   * until the next `getBitmap` call.
   */
  const uint8_t* getBitmap(uint32_t cp) const;

 private:
  explicit EpdStreamFont(EpdFontData* data, EpdFont* allocatedBlock, const char* path, size_t cacheSize);

  // The base class font data pointer (points INTO allocatedBlock_).
  EpdFontData* allocatedData_;

  // The start of the unified heap_caps_malloc block from EpdFontSerializer.
  // This is what must be freed — not allocatedData_ directly.
  EpdFont* allocatedBlock_;

  std::string path_;

  // --- LRU Cache State ---
  size_t maxCacheSize_;
  mutable size_t currentCacheSize_;

  struct CacheNode {
    uint32_t cp;
    uint32_t lastAccessTick;
    std::vector<uint8_t> buffer;
  };

  mutable std::vector<CacheNode> cache_;
  mutable uint32_t accessTick_;

  void evictOldest() const;
};

#endif  // ENABLE_CUSTOM_FONTS
