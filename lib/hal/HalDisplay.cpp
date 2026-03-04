#include <HalDisplay.h>
#include <HalGPIO.h>

#define SD_SPI_MISO 7

HalDisplay::HalDisplay() {}

HalDisplay::~HalDisplay() {}

void HalDisplay::begin() {
  bbep.setPanelType(EP426_800x480_4GRAY);

  // initIO args: iDC, iReset, iBusy, iCS, iMOSI, iSCLK, u32Speed
  bbep.initIO(EPD_DC, EPD_RST, EPD_BUSY, EPD_CS, EPD_MOSI, EPD_SCLK, 40000000);
  bbep.allocBuffer();
}

void HalDisplay::clearScreen(uint8_t color) const { const_cast<BBEPAPER*>(&bbep)->fillScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  // Optional: implement if needed by reading image bytes into bbep
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  // Optional: implement if needed
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  bbep.writePlane();
  bbep.refresh(mode);
  if (turnOffScreen) deepSleep();
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  displayBuffer(mode, turnOffScreen);
}

void HalDisplay::deepSleep() { bbep.sleep(0); }

// Graphical Primitives delegated to bb_epaper
void HalDisplay::drawPixel(int16_t x, int16_t y, uint8_t color) { bbep.drawPixel(x, y, color); }
void HalDisplay::drawLine(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint8_t color) {
  bbep.drawLine(x1, y1, x2, y2, color);
}
void HalDisplay::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
  bbep.drawRect(x, y, w, h, color);
}
void HalDisplay::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
  bbep.fillRect(x, y, w, h, color);
}
void HalDisplay::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t color) {
  bbep.drawRoundRect(x, y, w, h, r, color);
}
void HalDisplay::fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t color) {
  bbep.fillRoundRect(x, y, w, h, r, color);
}
void HalDisplay::drawCircle(int32_t x, int32_t y, int32_t r, uint8_t color) { bbep.drawCircle(x, y, r, color); }
void HalDisplay::fillCircle(int32_t x, int32_t y, int32_t r, uint8_t color) { bbep.fillCircle(x, y, r, color); }
void HalDisplay::drawEllipse(int16_t x, int16_t y, int32_t rx, int32_t ry, uint8_t color) {
  bbep.drawEllipse(x, y, rx, ry, color);
}
void HalDisplay::fillEllipse(int16_t x, int16_t y, int32_t rx, int32_t ry, uint8_t color) {
  bbep.fillEllipse(x, y, rx, ry, color);
}
void HalDisplay::setRotation(int rotation) { bbep.setRotation(rotation); }
