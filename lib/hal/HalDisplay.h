#pragma once
#include <Arduino.h>
#include <bb_epaper.h>

class HalDisplay {
 public:
  // Constructor
  HalDisplay();

  // Destructor
  ~HalDisplay();

  // Refresh modes
  enum RefreshMode { FULL_REFRESH = REFRESH_FULL, FAST_REFRESH = REFRESH_FAST, PARTIAL_REFRESH = REFRESH_PARTIAL };

  // bb_epaper color constants wrapper
  // For EP426 4-gray, u8Colors_4gray mapping table is inverted internally (0->3, 1->2, 2->1, 3->0).
  // The panel requires physical 0 for White, 3 for Black.
  // Therefore, pass 3 for White, 2 for Light Gray, 1 for Dark Gray, 0 for Black.
  enum Color : uint8_t { Black = 0, White = 3, Gray1 = 2, Gray2 = 1, Gray3 = 1 };

  // Initialize the display hardware and driver
  void begin();

  // Display dimensions
  static constexpr uint16_t DISPLAY_WIDTH = 800;
  static constexpr uint16_t DISPLAY_HEIGHT = 480;

  // Frame buffer operations
  void clearScreen(uint8_t color = BBEP_WHITE) const;

  // NOTE: drawImage/drawImageTransparent signature might need adaptation depending on how GfxRenderer uses them later,
  // but we provide basic placeholders for bb_epaper mapping (bb_epaper has loadBMP etc or custom methods).
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 bool fromProgmem = false) const;
  void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            bool fromProgmem = false) const;

  // Render and update
  void displayBuffer(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  void refreshDisplay(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);

  // Power management
  void deepSleep();

  // Graphical Primitives delegated to bb_epaper
  void drawPixel(int16_t x, int16_t y, uint8_t color);
  void drawLine(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint8_t color);
  void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);
  void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t color);
  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t color);
  void drawCircle(int32_t x, int32_t y, int32_t r, uint8_t color);
  void fillCircle(int32_t x, int32_t y, int32_t r, uint8_t color);
  void drawEllipse(int16_t x, int16_t y, int32_t rx, int32_t ry, uint8_t color);
  void fillEllipse(int16_t x, int16_t y, int32_t rx, int32_t ry, uint8_t color);

  void setRotation(int rotation);

  // Directly access underlying BBEPAPER instance for advanced usage if needed
  BBEPAPER* getDriver() { return &bbep; }

 private:
  BBEPAPER bbep;
};
