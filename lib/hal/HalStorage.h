#pragma once

#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <SDCardManager.h>

#include <vector>

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

// --- Type compatibility aliases ---
// FsFile is SdFat's own type again. The SDK mounts an FsVolume on a native
// esp-idf SDMMC block device (SdFat has no SDIO driver of its own), so this
// board gets real SdFat file objects even though its card is on 4-bit SDMMC
// rather than SPI — which is what upstream already assumes.
//
// This retires the EspFsFile shim, which wrapped ESP32's fs::File and re-added
// the SdFat methods Arduino's File lacks (seekCur, int read(), void* read/write,
// getName, seekSet, fileSize, ...). FsFile has all of them natively.
//
// EspFsFile stays as an alias so the port's existing references keep compiling;
// new code should use FsFile.
using EspFsFile = FsFile;
using HalFile = FsFile;

// Downstream code must use Storage instead of SdMan
#ifdef SdMan
#undef SdMan
#endif
