#pragma once

class EspFsFile;
class Print;

class PngToBmpConverter {
  static bool pngFileToBmpStreamInternal(EspFsFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight, bool oneBit,
                                         bool crop = true);

 public:
  static bool pngFileToBmpStream(EspFsFile& pngFile, Print& bmpOut, bool crop = true);
  static bool pngFileToBmpStreamWithSize(EspFsFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  static bool pngFileTo1BitBmpStreamWithSize(EspFsFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
};
