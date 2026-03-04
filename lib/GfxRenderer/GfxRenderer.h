#pragma once

#include <EpdFontFamily.h>
#include <FontDecompressor.h>
#include <HalDisplay.h>

#include <map>
#include <string>
#include <vector>

#include "Bitmap.h"

// Color representation mapped to bb_epaper levels (0-3 + transparent)
enum Color : uint8_t {
  Clear = BBEP_TRANSPARENT,
  White = HalDisplay::White,
  LightGray = HalDisplay::Gray1,
  DarkGray = HalDisplay::Gray2,
  Black = HalDisplay::Black
};

class GfxRenderer {
 public:
  // Logical screen orientation from the perspective of callers
  enum Orientation {
    Portrait,                  // 480x800 logical coordinates (current default)
    LandscapeClockwise,        // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    PortraitInverted,          // 480x800 logical coordinates, inverted
    LandscapeCounterClockwise  // 800x480 logical coordinates, native panel orientation
  };

 public:
  HalDisplay& getDisplay() const { return display; }

 private:
  HalDisplay& display;
  Orientation orientation;
  bool fadingFix;
  std::map<int, EpdFontFamily> fontMap;
  FontDecompressor* fontDecompressor = nullptr;

  template <Color color>
  void drawPixelDither(int x, int y) const;
  template <Color color>
  void fillArc(int maxRadius, int cx, int cy, int xDir, int yDir) const;

 public:
  explicit GfxRenderer(HalDisplay& halDisplay) : display(halDisplay), orientation(Portrait), fadingFix(false) {}
  ~GfxRenderer() = default;

  static constexpr int VIEWABLE_MARGIN_TOP = 0;
  static constexpr int VIEWABLE_MARGIN_RIGHT = 0;
  static constexpr int VIEWABLE_MARGIN_BOTTOM = 0;
  static constexpr int VIEWABLE_MARGIN_LEFT = 0;

  // Setup
  void begin();  // must be called right after display.begin()
  void insertFont(int fontId, EpdFontFamily font);
  void setFontDecompressor(FontDecompressor* d) { fontDecompressor = d; }
  void clearFontCache() {
    if (fontDecompressor) fontDecompressor->clearCache();
  }

  // Orientation control (affects logical width/height and coordinate transforms)
  void setOrientation(const Orientation o);
  Orientation getOrientation() const { return orientation; }

  // Fading fix control (legacy API retained for compat, functionality handled natively by bb_epaper)
  void setFadingFix(const bool enabled) { fadingFix = enabled; }

  // Screen ops
  int getScreenWidth() const;
  int getScreenHeight() const;
  void displayBuffer(HalDisplay::RefreshMode refreshMode = HalDisplay::PARTIAL_REFRESH) const;
  void displayGrayBuffer() const;
  void invertScreen() const;
  void clearScreen(uint8_t color = HalDisplay::White) const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;

  // Drawing
  void drawPixel(int x, int y, Color color = Color::Black) const;
  // Legacy 1-bit compat
  void drawPixel(int x, int y, bool state) const;

  void drawLine(int x1, int y1, int x2, int y2, Color color = Color::Black) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state) const;

  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, Color color) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;

  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const;
  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, Color color) const;

  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, Color color) const;

  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, Color color) const;

  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, Color color) const;

  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool roundTopLeft,
                       bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool roundTopLeft,
                       bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, Color color) const;

  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void fillRect(int x, int y, int width, int height, Color color) const;

  void fillRectDither(int x, int y, int width, int height, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft, bool roundTopRight,
                       bool roundBottomLeft, bool roundBottomRight, Color color) const;

  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0,
                  float cropY = 0) const;
  void drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight) const;

  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state = true) const;
  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, Color color) const;

  // Text
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawText(int fontId, int x, int y, const char* text, Color color,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getSpaceKernAdjust(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
  std::string truncatedText(int fontId, const char* text, int maxWidth,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  std::vector<std::string> wrappedText(int fontId, const char* text, int maxWidth, int maxLines,
                                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  void drawTextRotated90CW(int fontId, int x, int y, const char* text, bool black = true,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextHeight(int fontId) const;

  // Font helpers
  const uint8_t* getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const;
};
