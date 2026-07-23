#pragma once

#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <SDCardManager.h>

#include <utility>  // std::move (HalFile is move-only)
#include <vector>

// --- File type ---
// The card now yields real SdFat FsFile objects: the SDK mounts an FsVolume on a
// native esp-idf SDMMC block device (SdFat has no SDIO driver of its own), so
// this board gets the same file type upstream assumes despite being on 4-bit
// SDMMC rather than SPI. That retired the old EspFsFile shim, which wrapped
// ESP32's fs::File to re-add the SdFat methods Arduino's File lacks (seekCur,
// int read(), void* read/write, getName, seekSet, fileSize, ...) — FsFile has
// all of those natively.
//
// Two names remain SdFat's own spelling rather than upstream's: FsFile calls
// them isDir() and rewind(), while upstream CrossPoint (and therefore this
// port's ~30 call sites) says isDirectory() and rewindDirectory(). Upstream
// reconciles that with its own HalFile wrapper class; this is the same idea
// pared down to the two names, since the ESP32 SD driver is already thread-safe
// and needs none of upstream's mutex machinery.
//
// openNextFile() is shadowed so directory walks stay in this type — otherwise
// `HalFile entry = dir.openNextFile()` would hand back a plain FsFile and lose
// isDirectory() on the very call that needs it.
class HalFile : public FsFile {
 public:
  HalFile() = default;
  // Move-only: SdFat builds with FILE_COPY_CONSTRUCTOR_PRIVATE, so a file object
  // can be moved but never copied. Matches how FsFile itself behaves.
  HalFile(FsFile&& other) : FsFile(std::move(other)) {}
  HalFile& operator=(FsFile&& other) {
    FsFile::operator=(std::move(other));
    return *this;
  }

  [[nodiscard]] bool isDirectory() const { return isDir(); }
  void rewindDirectory() { rewind(); }

  // Upstream's explicit 64-bit spellings. SdFat is already 64-bit throughout --
  // fileSize() returns uint64_t and seekSet() takes one -- so these are pure
  // naming, kept so the call sites read the same as upstream's.
  [[nodiscard]] uint64_t fileSize64() { return fileSize(); }
  bool seek64(uint64_t pos) { return seekSet(pos); }

  [[nodiscard]] HalFile openNextFile(oflag_t oflag = O_RDONLY) {
    HalFile next;
    next.openNext(this, oflag);
    return next;
  }
};

// The port's existing references; new code should use HalFile.
using EspFsFile = HalFile;

class HalStorage {
 public:
  HalStorage();
  bool begin();
  bool ready() const;
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  // Read the entire file at `path` into a String. Returns empty string on failure.
  String readFile(const char* path);
  // Low-memory helpers:
  // Stream the file contents to a `Print` (e.g. `Serial`, or any `Print`-derived object).
  // Returns true on success, false on failure.
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256);
  // Read up to `bufferSize-1` bytes into `buffer`, null-terminating it. Returns bytes read.
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  // Write a string to `path` on the SD card. Overwrites existing file.
  // Returns true on success.
  bool writeFile(const char* path, const String& content);
  // Ensure a directory exists, creating it if necessary. Returns true on success.
  bool ensureDirectoryExists(const char* path);

  EspFsFile open(const char* path, const oflag_t oflag = O_RDONLY);
  bool mkdir(const char* path, const bool pFlag = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool rmdir(const char* path);

  bool openFileForRead(const char* moduleName, const char* path, EspFsFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, EspFsFile& file);
  bool openFileForRead(const char* moduleName, const String& path, EspFsFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, EspFsFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, EspFsFile& file);
  bool openFileForWrite(const char* moduleName, const String& path, EspFsFile& file);
  bool removeDir(const char* path);

  static HalStorage& getInstance() { return instance; }

 private:
  static HalStorage instance;

  bool initialized = false;
};

#define Storage HalStorage::getInstance()


// Downstream code must use Storage instead of SdMan
#ifdef SdMan
#undef SdMan
#endif
