#pragma once

/**
 * FsCompat.h
 *
 * Provides a SdFat-compatible file type (EspFsFile) built on top of ESP32's
 * native fs::File (from the FS / SD_MMC libraries).
 *
 * This shim bridges the following SdFat-specific API calls that do not exist
 * on fs::File:
 *
 *  - seekCur(offset)       → seek relative to current position
 *  - read()                → single-byte read returning int (-1 on EOF)
 *  - read(buf, size)       → buffer read accepting void* (not just uint8_t*)
 *  - write(buf, size)      → buffer write accepting const void* / const uint8_t*
 *  - seek(pos)             → seeks to absolute position, returns bool
 *  - position()            → current position (already on fs::File)
 *  - size()                → file size (already on fs::File)
 *
 * Usage: just #include <FsCompat.h> wherever SdFat.h was previously included.
 * EspFsFile is now this wrapper type, so existing code compiles as-is.
 */

#include <FS.h>
#include <SD_MMC.h>  // needed for rename() implementation

// Flag constants matching SdFat's oflag_t values (POSIX-compatible)
#include <fcntl.h>  // Provides O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_TRUNC, O_APPEND
typedef int oflag_t;

class EspFsFile : public fs::File {
 public:
  // Default constructor
  EspFsFile() = default;

  // Construct from an existing fs::File (e.g. from SD_MMC.open())
  explicit EspFsFile(fs::File&& f) : fs::File(std::move(f)) {}

  // Allow assignment from fs::File
  EspFsFile& operator=(fs::File&& f) {
    fs::File::operator=(std::move(f));
    return *this;
  }

  // -------------------------------------------------------------------------
  // seek() — absolute seek, returns bool (true = success), SdFat compatible
  // -------------------------------------------------------------------------
  bool seek(uint32_t pos) { return fs::File::seek(pos); }

  // -------------------------------------------------------------------------
  // seekCur() — relative seek. SdFat has this; fs::File does not.
  // -------------------------------------------------------------------------
  bool seekCur(int32_t offset) {
    const size_t cur = fs::File::position();
    const long newPos = static_cast<long>(cur) + offset;
    if (newPos < 0) return false;
    return fs::File::seek(static_cast<uint32_t>(newPos));
  }

  // -------------------------------------------------------------------------
  // fileSize() — SdFat name for what ESP32 calls size()
  // -------------------------------------------------------------------------
  size_t fileSize() { return fs::File::size(); }

  // -------------------------------------------------------------------------
  // seekSet() — SdFat name for absolute seek (same as seek())
  // -------------------------------------------------------------------------
  bool seekSet(uint32_t pos) { return fs::File::seek(pos); }

  // -------------------------------------------------------------------------
  // read() with no args — SdFat returns int (byte value, or -1 on EOF).
  // fs::File::read() with no args also returns int, so this isn't strictly
  // needed, but is here for clarity.
  // -------------------------------------------------------------------------
  int read() { return fs::File::read(); }

  // -------------------------------------------------------------------------
  // read(buf, size) — SdFat takes void*; fs::File takes uint8_t*.
  // We bridge the types here so callers can pass char* or void*.
  // -------------------------------------------------------------------------
  size_t read(void* buf, size_t size) { return fs::File::read(static_cast<uint8_t*>(buf), size); }

  size_t read(uint8_t* buf, size_t size) { return fs::File::read(buf, size); }

  size_t read(char* buf, size_t size) { return fs::File::read(reinterpret_cast<uint8_t*>(buf), size); }

  // -------------------------------------------------------------------------
  // write() — single byte. The base class has this, but our overloads below
  // hide it. Explicitly re-expose it.
  // -------------------------------------------------------------------------
  size_t write(uint8_t b) { return fs::File::write(b); }
  size_t write(char c) { return fs::File::write(static_cast<uint8_t>(c)); }

  // -------------------------------------------------------------------------
  // write(buf, size) — SdFat takes const void*; fs::File takes const uint8_t*.
  // -------------------------------------------------------------------------
  size_t write(const void* buf, size_t size) { return fs::File::write(static_cast<const uint8_t*>(buf), size); }

  size_t write(const uint8_t* buf, size_t size) { return fs::File::write(buf, size); }

  size_t write(const char* buf, size_t size) { return fs::File::write(reinterpret_cast<const uint8_t*>(buf), size); }

  // -------------------------------------------------------------------------
  // getName() / getAbsolutePath() — provide a SdFat-like name accessor.
  // fs::File::name() returns the filename (last segment).
  // -------------------------------------------------------------------------
  bool getName(char* name, size_t len) const {
    const char* n = fs::File::name();
    if (!n) return false;
    strncpy(name, n, len - 1);
    name[len - 1] = '\0';
    return true;
  }

  // -------------------------------------------------------------------------
  // rename() — SdFat has file.rename(newPath); fs::File doesn't.
  // We use SD_MMC.rename() for the actual operation.
  // -------------------------------------------------------------------------
  bool rename(const char* newPath) {
    const char* oldPath = fs::File::path();
    if (!oldPath) return false;
    return SD_MMC.rename(oldPath, newPath);
  }

  // -------------------------------------------------------------------------
  // isOpen() — SdFat has this; on fs::File just check operator bool()
  // -------------------------------------------------------------------------
  bool isOpen() const { return operator bool(); }

  // -------------------------------------------------------------------------
  // openNextFile() — SdFat's openNextFile() returns an EspFsFile; fs::File's
  // returns an fs::File. We override here to return EspFsFile so the iterator
  // pattern `for(auto file = dir.openNextFile(); file; ...)` works correctly
  // and the resulting files have `getName()` available.
  // -------------------------------------------------------------------------
  EspFsFile openNextFile() { return EspFsFile(fs::File::openNextFile()); }

  // -------------------------------------------------------------------------
  // rewindDirectory() — identical to base class, but present for clarity
  // -------------------------------------------------------------------------
  void rewindDirectory() { fs::File::rewindDirectory(); }

  // -------------------------------------------------------------------------
  // close() — already on fs::File, forward for compatibility
  // -------------------------------------------------------------------------
  void close() { fs::File::close(); }
};
