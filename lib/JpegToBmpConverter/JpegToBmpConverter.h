#pragma once

class EspFsFile;
class Print;
class ZipFile;

class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(EspFsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop = true);

 public:
  static bool jpegFileToBmpStream(EspFsFile& jpegFile, Print& bmpOut, bool crop = true);
  // Convert with custom target size (for thumbnails)
  static bool jpegFileToBmpStreamWithSize(EspFsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(EspFsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                              int targetMaxHeight);
};
