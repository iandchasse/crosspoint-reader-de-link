#include "GfxRenderer.h"

#include <Logging.h>
#include <Utf8.h>

const uint8_t* GfxRenderer::getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const {
  if (fontData->groups != nullptr) {
    if (!fontDecompressor) {
      LOG_ERR("GFX", "Compressed font but no FontDecompressor set");
      return nullptr;
    }
    uint16_t glyphIndex = static_cast<uint16_t>(glyph - fontData->glyph);
    return fontDecompressor->getBitmap(fontData, glyph, glyphIndex);
  }
  return &fontData->bitmap[glyph->dataOffset];
}

void GfxRenderer::begin() {
  // Initialization handled by HalDisplay::begin()
}

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) { fontMap.insert({fontId, font}); }

void GfxRenderer::setOrientation(const Orientation o) {
  orientation = o;
  switch (o) {
    case LandscapeCounterClockwise:
      display.setRotation(0);
      break;
    case PortraitInverted:
      display.setRotation(270);
      break;
    case LandscapeClockwise:
      display.setRotation(180);
      break;
    case Portrait:
      display.setRotation(90);
      break;
  }
}

enum class TextRotation { None, Rotated90CW };

template <TextRotation rotation>
static void renderCharImpl(const GfxRenderer& renderer, const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX,
                           int cursorY, Color color, const EpdFontFamily::Style style) {
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) {
    LOG_ERR("GFX", "No glyph for codepoint %d", cp);
    return;
  }

  const EpdFontData* fontData = fontFamily.getData(style);
  const bool is2Bit = fontData->is2Bit;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;
  const int top = glyph->top;

  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);

  if (bitmap != nullptr) {
    int outerBase, innerBase;
    if constexpr (rotation == TextRotation::Rotated90CW) {
      outerBase = cursorX + fontData->ascender - top;
      innerBase = cursorY - left;
    } else {
      outerBase = cursorY - top;
      innerBase = cursorX + left;
    }

    if (is2Bit) {
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 2];
          const uint8_t bit_index = (3 - (pixelPosition & 3)) * 2;
          const uint8_t bmpVal = 3 - ((byte >> bit_index) & 0x3);

          if (bmpVal < 3) {  // not white
            Color drawColor = color;
            if (color == Color::Black) {
              if (bmpVal == 0)
                drawColor = Color::Black;
              else if (bmpVal == 1)
                drawColor = Color::DarkGray;
              else if (bmpVal == 2)
                drawColor = Color::LightGray;
            }
            renderer.drawPixel(screenX, screenY, drawColor);
          }
        }
      }
    } else {
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 3];
          const uint8_t bit_index = 7 - (pixelPosition & 7);

          if ((byte >> bit_index) & 1) {
            renderer.drawPixel(screenX, screenY, color);
          }
        }
      }
    }
  }
}

void GfxRenderer::drawPixel(const int x, const int y, Color color) const {
  display.drawPixel(x, y, static_cast<uint8_t>(color));
}

void GfxRenderer::drawPixel(const int x, const int y, bool state) const {
  drawPixel(x, y, state ? Color::Black : Color::White);
}

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }
  int w = 0, h = 0;
  fontIt->second.getTextDimensions(text, &w, &h, style);
  return w;
}

void GfxRenderer::drawCenteredText(const int fontId, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style) const {
  const int x = (getScreenWidth() - getTextWidth(fontId, text, style)) / 2;
  drawText(fontId, x, y, text, black ? Color::Black : Color::White, style);
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style) const {
  drawText(fontId, x, y, text, black ? Color::Black : Color::White, style);
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, Color color,
                           const EpdFontFamily::Style style) const {
  const int yPos = y + getFontAscenderSize(fontId);
  int32_t xPosFP = fp4::fromPixel(x);
  int lastBaseX = x;
  int lastBaseAdvanceFP = 0;
  int lastBaseTop = 0;

  if (text == nullptr || *text == '\0') return;

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return;
  }
  const auto& font = fontIt->second;
  constexpr int MIN_COMBINING_GAP_PX = 1;

  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      int raiseBy = 0;
      if (combiningGlyph) {
        const int currentGap = combiningGlyph->top - combiningGlyph->height - lastBaseTop;
        if (currentGap < MIN_COMBINING_GAP_PX) raiseBy = MIN_COMBINING_GAP_PX - currentGap;
      }

      const int combiningX = lastBaseX + fp4::toPixel(lastBaseAdvanceFP / 2);
      const int combiningY = yPos - raiseBy;
      renderCharImpl<TextRotation::None>(*this, font, cp, combiningX, combiningY, color, style);
      continue;
    }

    cp = font.applyLigatures(cp, text, style);
    const int kernFP = (prevCp != 0) ? font.getKerning(prevCp, cp, style) : 0;
    xPosFP += kernFP;

    lastBaseX = fp4::toPixel(xPosFP);
    const EpdGlyph* glyph = font.getGlyph(cp, style);

    lastBaseAdvanceFP = glyph ? glyph->advanceX : 0;
    lastBaseTop = glyph ? glyph->top : 0;

    renderCharImpl<TextRotation::None>(*this, font, cp, lastBaseX, yPos, color, style);
    if (glyph) xPosFP += glyph->advanceX;
    prevCp = cp;
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
  drawLine(x1, y1, x2, y2, state ? Color::Black : Color::White);
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, Color color) const {
  display.drawLine(x1, y1, x2, y2, static_cast<uint8_t>(color));
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const int lineWidth, Color color) const {
  for (int i = 0; i < lineWidth; i++) {
    display.drawLine(x1, y1 + i, x2, y2 + i, static_cast<uint8_t>(color));
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const {
  drawLine(x1, y1, x2, y2, lineWidth, state ? Color::Black : Color::White);
}

void GfxRenderer::drawArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir,
                          const int lineWidth, const bool state) const {
  drawArc(maxRadius, cx, cy, xDir, yDir, lineWidth, state ? Color::Black : Color::White);
}

void GfxRenderer::drawArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir,
                          const int lineWidth, Color color) const {
  const int stroke = std::min(lineWidth, maxRadius);
  const int innerRadius = std::max(maxRadius - stroke, 0);
  const int outerRadiusSq = maxRadius * maxRadius;
  const int innerRadiusSq = innerRadius * innerRadius;
  for (int dy = 0; dy <= maxRadius; ++dy) {
    for (int dx = 0; dx <= maxRadius; ++dx) {
      const int distSq = dx * dx + dy * dy;
      if (distSq > outerRadiusSq || distSq < innerRadiusSq) continue;
      const int px = cx + xDir * dx;
      const int py = cy + yDir * dy;
      drawPixel(px, py, color);
    }
  }
}

void GfxRenderer::drawRect(int x, int y, int width, int height, bool state) const {
  drawRect(x, y, width, height, state ? Color::Black : Color::White);
}

void GfxRenderer::drawRect(int x, int y, int width, int height, Color color) const {
  display.drawRect(x, y, width, height, static_cast<uint8_t>(color));
}

void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const int lineWidth,
                           const bool state) const {
  drawRect(x, y, width, height, lineWidth, state ? Color::Black : Color::White);
}

void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const int lineWidth,
                           Color color) const {
  for (int i = 0; i < lineWidth; i++) {
    drawRect(x + i, y + i, width - i * 2, height - i * 2, color);
  }
}

void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool state) const {
  drawRoundedRect(x, y, width, height, lineWidth, cornerRadius, true, true, true, true, state);
}

void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, Color color) const {
  drawRoundedRect(x, y, width, height, lineWidth, cornerRadius, true, true, true, true, color);
}

void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool roundTopLeft, bool roundTopRight, bool roundBottomLeft,
                                  bool roundBottomRight, bool state) const {
  drawRoundedRect(x, y, width, height, lineWidth, cornerRadius, roundTopLeft, roundTopRight, roundBottomLeft,
                  roundBottomRight, state ? Color::Black : Color::White);
}

void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool roundTopLeft, bool roundTopRight, bool roundBottomLeft,
                                  bool roundBottomRight, Color color) const {
  if (lineWidth <= 0 || width <= 0 || height <= 0) return;
  const int maxRadius = std::min({cornerRadius, width / 2, height / 2});
  if (maxRadius <= 0) {
    drawRect(x, y, width, height, lineWidth, color);
    return;
  }
  const int stroke = std::min(lineWidth, maxRadius);
  const int right = x + width - 1;
  const int bottom = y + height - 1;
  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    if (roundTopLeft || roundTopRight) fillRect(x + maxRadius, y, horizontalWidth, stroke, color);
    if (roundBottomLeft || roundBottomRight)
      fillRect(x + maxRadius, bottom - stroke + 1, horizontalWidth, stroke, color);
  }
  const int verticalHeight = height - 2 * maxRadius;
  if (verticalHeight > 0) {
    if (roundTopLeft || roundBottomLeft) fillRect(x, y + maxRadius, stroke, verticalHeight, color);
    if (roundTopRight || roundBottomRight) fillRect(right - stroke + 1, y + maxRadius, stroke, verticalHeight, color);
  }
  if (roundTopLeft) drawArc(maxRadius, x + maxRadius, y + maxRadius, -1, -1, lineWidth, color);
  if (roundTopRight) drawArc(maxRadius, right - maxRadius, y + maxRadius, 1, -1, lineWidth, color);
  if (roundBottomRight) drawArc(maxRadius, right - maxRadius, bottom - maxRadius, 1, 1, lineWidth, color);
  if (roundBottomLeft) drawArc(maxRadius, x + maxRadius, bottom - maxRadius, -1, 1, lineWidth, color);
}

void GfxRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  fillRect(x, y, width, height, state ? Color::Black : Color::White);
}

void GfxRenderer::fillRect(const int x, const int y, const int width, const int height, Color color) const {
  display.fillRect(x, y, width, height, static_cast<uint8_t>(color));
}

void GfxRenderer::fillRectDither(const int x, const int y, const int width, const int height, Color color) const {
  if (color != Color::Clear) {
    fillRect(x, y, width, height, color);
  }
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  const Color color) const {
  fillRoundedRect(x, y, width, height, cornerRadius, true, true, true, true, color);
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  bool roundTopLeft, bool roundTopRight, bool roundBottomLeft, bool roundBottomRight,
                                  const Color color) const {
  if (width <= 0 || height <= 0) return;
  const int roundedSides = (!roundTopLeft || !roundTopRight || !roundBottomLeft || !roundBottomRight) ? 1 : 2;
  const int maxRadius = std::min({cornerRadius, width / roundedSides, height / roundedSides});
  if (maxRadius <= 0) {
    fillRect(x, y, width, height, color);
    return;
  }
  if (roundTopLeft && roundTopRight && roundBottomLeft && roundBottomRight) {
    display.fillRoundRect(x, y, width, height, maxRadius, static_cast<uint8_t>(color));
    return;
  }

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) fillRect(x + maxRadius + 1, y, horizontalWidth - 2, height, color);
  const int leftFillTop = y + (roundTopLeft ? (maxRadius + 1) : 0);
  const int leftFillBottom = y + height - 1 - (roundBottomLeft ? (maxRadius + 1) : 0);
  if (leftFillBottom >= leftFillTop) fillRect(x, leftFillTop, maxRadius + 1, leftFillBottom - leftFillTop + 1, color);
  const int rightFillTop = y + (roundTopRight ? (maxRadius + 1) : 0);
  const int rightFillBottom = y + height - 1 - (roundBottomRight ? (maxRadius + 1) : 0);
  if (rightFillBottom >= rightFillTop)
    fillRect(x + width - maxRadius - 1, rightFillTop, maxRadius + 1, rightFillBottom - rightFillTop + 1, color);

  auto fillArcFn = [this](int maxRadius, int cx, int cy, int xDir, int yDir, Color color) {
    const int radiusSq = maxRadius * maxRadius;
    for (int dy = 0; dy <= maxRadius; ++dy) {
      for (int dx = 0; dx <= maxRadius; ++dx) {
        if (dx * dx + dy * dy <= radiusSq) {
          drawPixel(cx + xDir * dx, cy + yDir * dy, color);
        }
      }
    }
  };

  if (roundTopLeft) fillArcFn(maxRadius, x + maxRadius, y + maxRadius, -1, -1, color);
  if (roundTopRight) fillArcFn(maxRadius, x + width - maxRadius - 1, y + maxRadius, 1, -1, color);
  if (roundBottomRight) fillArcFn(maxRadius, x + width - maxRadius - 1, y + height - maxRadius - 1, 1, 1, color);
  if (roundBottomLeft) fillArcFn(maxRadius, x + maxRadius, y + height - maxRadius - 1, -1, 1, color);
}

void GfxRenderer::drawImage(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  for (int r = 0; r < height; r++) {
    int rowIdx = r * (width / 8);
    for (int c = 0; c < width; c++) {
      if ((bitmap[rowIdx + (c / 8)] & (0x80 >> (c % 8))) == 0) {
        drawPixel(x + c, y + r, Color::Black);
      } else {
        drawPixel(x + c, y + r, Color::White);
      }
    }
  }
}

void GfxRenderer::drawIcon(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  for (int r = 0; r < height; r++) {
    int rowIdx = r * (width / 8);
    for (int c = 0; c < width; c++) {
      if ((bitmap[rowIdx + (c / 8)] & (0x80 >> (c % 8))) == 0) {
        drawPixel(x + height - 1 - r, y + c, Color::Black);
      }
    }
  }
}

void GfxRenderer::drawBitmap(const Bitmap& bitmap, const int x, const int y, const int maxWidth, const int maxHeight,
                             const float cropX, const float cropY) const {
  if (bitmap.is1Bit() && cropX == 0.0f && cropY == 0.0f) {
    drawBitmap1Bit(bitmap, x, y, maxWidth, maxHeight);
    return;
  }

  float scale = 1.0f;
  bool isScaled = false;
  int cropPixX = std::floor(bitmap.getWidth() * cropX / 2.0f);
  int cropPixY = std::floor(bitmap.getHeight() * cropY / 2.0f);

  if (maxWidth > 0 && (1.0f - cropX) * bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>((1.0f - cropX) * bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && (1.0f - cropY) * bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>((1.0f - cropY) * bitmap.getHeight()));
    isScaled = true;
  }

  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* rowBytes = static_cast<uint8_t*>(malloc(bitmap.getRowBytes()));

  if (!outputRow || !rowBytes) return;

  for (int bmpY = 0; bmpY < (bitmap.getHeight() - cropPixY); bmpY++) {
    int screenY = -cropPixY + (bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY);
    if (isScaled) screenY = std::floor(screenY * scale);
    screenY += y;
    if (screenY >= getScreenHeight()) break;

    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      free(outputRow);
      free(rowBytes);
      return;
    }

    if (screenY < 0 || bmpY < cropPixY) continue;

    for (int bmpX = cropPixX; bmpX < bitmap.getWidth() - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) screenX = std::floor(screenX * scale);
      screenX += x;
      if (screenX >= getScreenWidth()) break;
      if (screenX < 0) continue;

      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;
      if (val == 0)
        drawPixel(screenX, screenY, Color::White);
      else if (val == 1)
        drawPixel(screenX, screenY, Color::LightGray);
      else if (val == 2)
        drawPixel(screenX, screenY, Color::DarkGray);
      else
        drawPixel(screenX, screenY, Color::Black);
    }
  }

  free(outputRow);
  free(rowBytes);
}

void GfxRenderer::drawBitmap1Bit(const Bitmap& bitmap, const int x, const int y, const int maxWidth,
                                 const int maxHeight) const {
  float scale = 1.0f;
  bool isScaled = false;
  if (maxWidth > 0 && bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>(bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>(bitmap.getHeight()));
    isScaled = true;
  }

  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* rowBytes = static_cast<uint8_t*>(malloc(bitmap.getRowBytes()));

  if (!outputRow || !rowBytes) return;

  for (int bmpY = 0; bmpY < bitmap.getHeight(); bmpY++) {
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) break;

    const int bmpYOffset = bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY;
    int screenY = y + (isScaled ? static_cast<int>(std::floor(bmpYOffset * scale)) : bmpYOffset);
    if (screenY >= getScreenHeight()) continue;
    if (screenY < 0) continue;

    for (int bmpX = 0; bmpX < bitmap.getWidth(); bmpX++) {
      int screenX = x + (isScaled ? static_cast<int>(std::floor(bmpX * scale)) : bmpX);
      if (screenX >= getScreenWidth()) break;
      if (screenX < 0) continue;

      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;
      if (val >= 3) drawPixel(screenX, screenY, Color::Black);
    }
  }

  free(outputRow);
  free(rowBytes);
}

void GfxRenderer::fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state) const {
  fillPolygon(xPoints, yPoints, numPoints, state ? Color::Black : Color::White);
}

void GfxRenderer::fillPolygon(const int* xPoints, const int* yPoints, int numPoints, Color color) const {
  if (numPoints < 3) return;

  int minY = yPoints[0], maxY = yPoints[0];
  for (int i = 1; i < numPoints; i++) {
    if (yPoints[i] < minY) minY = yPoints[i];
    if (yPoints[i] > maxY) maxY = yPoints[i];
  }

  if (minY < 0) minY = 0;
  if (maxY >= getScreenHeight()) maxY = getScreenHeight() - 1;

  auto* nodeX = static_cast<int*>(malloc(numPoints * sizeof(int)));
  if (!nodeX) return;

  for (int scanY = minY; scanY <= maxY; scanY++) {
    int nodes = 0;
    int j = numPoints - 1;
    for (int i = 0; i < numPoints; i++) {
      if ((yPoints[i] < scanY && yPoints[j] >= scanY) || (yPoints[j] < scanY && yPoints[i] >= scanY)) {
        int dy = yPoints[j] - yPoints[i];
        if (dy != 0) {
          nodeX[nodes++] = xPoints[i] + (scanY - yPoints[i]) * (xPoints[j] - xPoints[i]) / dy;
        }
      }
      j = i;
    }

    for (int i = 0; i < nodes - 1; i++) {
      for (int k = i + 1; k < nodes; k++) {
        if (nodeX[i] > nodeX[k]) {
          int temp = nodeX[i];
          nodeX[i] = nodeX[k];
          nodeX[k] = temp;
        }
      }
    }

    for (int i = 0; i < nodes - 1; i += 2) {
      int startX = nodeX[i];
      int endX = nodeX[i + 1];

      if (startX < 0) startX = 0;
      if (endX >= getScreenWidth()) endX = getScreenWidth() - 1;

      drawLine(startX, scanY, endX, scanY, color);
    }
  }

  free(nodeX);
}

static unsigned long start_ms = 0;

void GfxRenderer::clearScreen(const uint8_t color) const {
  start_ms = millis();
  display.clearScreen(color == 0xFF ? HalDisplay::White : color);
}

void GfxRenderer::invertScreen() const {
  // To implement invertScreen efficiently without frame buffer access,
  // we could just draw a large white-on-black or do it properly if needed.
  // GDEQ0426T82 doesn't have an invert command usually, but we no longer manage the PB.
  // Left as no-op until bb_epaper has invert.
}

void GfxRenderer::displayBuffer(const HalDisplay::RefreshMode refreshMode) const {
  auto elapsed = millis() - start_ms;
  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);
  display.displayBuffer(refreshMode, fadingFix);
}

void GfxRenderer::displayGrayBuffer() const { display.refreshDisplay(HalDisplay::FAST_REFRESH, fadingFix); }

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style) const {
  if (!text || maxWidth <= 0) return "";
  std::string item = text;
  const char* ellipsis = "\xe2\x80\xa6";
  int textWidth = getTextWidth(fontId, item.c_str(), style);
  if (textWidth <= maxWidth) return item;
  while (!item.empty() && getTextWidth(fontId, (item + ellipsis).c_str(), style) >= maxWidth) {
    utf8RemoveLastChar(item);
  }
  return item.empty() ? ellipsis : item + ellipsis;
}

std::vector<std::string> GfxRenderer::wrappedText(const int fontId, const char* text, const int maxWidth,
                                                  const int maxLines, const EpdFontFamily::Style style) const {
  std::vector<std::string> lines;
  if (!text || maxWidth <= 0 || maxLines <= 0) return lines;
  std::string remaining = text;
  std::string currentLine;
  while (!remaining.empty()) {
    if (static_cast<int>(lines.size()) == maxLines - 1) {
      std::string lastContent = currentLine.empty() ? remaining : currentLine + " " + remaining;
      lines.push_back(truncatedText(fontId, lastContent.c_str(), maxWidth, style));
      return lines;
    }
    size_t spacePos = remaining.find(' ');
    std::string word;
    if (spacePos == std::string::npos) {
      word = remaining;
      remaining.clear();
    } else {
      word = remaining.substr(0, spacePos);
      remaining.erase(0, spacePos + 1);
    }
    std::string testLine = currentLine.empty() ? word : currentLine + " " + word;
    if (getTextWidth(fontId, testLine.c_str(), style) <= maxWidth) {
      currentLine = testLine;
    } else {
      if (!currentLine.empty()) {
        lines.push_back(currentLine);
        if (getTextWidth(fontId, word.c_str(), style) > maxWidth) {
          lines.push_back(truncatedText(fontId, word.c_str(), maxWidth, style));
          currentLine.clear();
          if (static_cast<int>(lines.size()) >= maxLines) return lines;
        } else {
          currentLine = word;
        }
      } else {
        lines.push_back(truncatedText(fontId, word.c_str(), maxWidth, style));
        return lines;
      }
    }
  }
  if (!currentLine.empty() && static_cast<int>(lines.size()) < maxLines) {
    lines.push_back(currentLine);
  }
  return lines;
}

int GfxRenderer::getScreenWidth() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      return HalDisplay::DISPLAY_HEIGHT;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      return HalDisplay::DISPLAY_WIDTH;
  }
  return HalDisplay::DISPLAY_HEIGHT;
}

int GfxRenderer::getScreenHeight() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      return HalDisplay::DISPLAY_WIDTH;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      return HalDisplay::DISPLAY_HEIGHT;
  }
  return HalDisplay::DISPLAY_WIDTH;
}

int GfxRenderer::getSpaceWidth(const int fontId, const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const EpdGlyph* spaceGlyph = fontIt->second.getGlyph(' ', style);
  return spaceGlyph ? fp4::toPixel(spaceGlyph->advanceX) : 0;
}

int GfxRenderer::getSpaceKernAdjust(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                                    const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const auto& font = fontIt->second;
  const int kernFP = font.getKerning(leftCp, ' ', style) + font.getKerning(' ', rightCp, style);
  return fp4::toPixel(kernFP);
}

int GfxRenderer::getKerning(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                            const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const int kernFP = fontIt->second.getKerning(leftCp, rightCp, style);
  return fp4::toPixel(kernFP);
}

int GfxRenderer::getTextAdvanceX(const int fontId, const char* text, EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  uint32_t cp;
  uint32_t prevCp = 0;
  int32_t widthFP = 0;
  const auto& font = fontIt->second;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) continue;
    cp = font.applyLigatures(cp, text, style);
    if (prevCp != 0) widthFP += font.getKerning(prevCp, cp, style);
    const EpdGlyph* glyph = font.getGlyph(cp, style);
    if (glyph) widthFP += glyph->advanceX;
    prevCp = cp;
  }
  return fp4::toPixel(widthFP);
}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  return fontIt->second.getData(EpdFontFamily::REGULAR)->advanceY;
}

int GfxRenderer::getTextHeight(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

void GfxRenderer::drawTextRotated90CW(const int fontId, const int x, const int y, const char* text, const bool black,
                                      const EpdFontFamily::Style style) const {
  if (text == nullptr || *text == '\0') return;
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return;
  const auto& font = fontIt->second;
  int32_t yPosFP = fp4::fromPixel(y);
  int lastBaseY = y;
  int lastBaseAdvanceFP = 0;
  int lastBaseTop = 0;
  constexpr int MIN_COMBINING_GAP_PX = 1;

  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      int raiseBy = 0;
      if (combiningGlyph) {
        const int currentGap = combiningGlyph->top - combiningGlyph->height - lastBaseTop;
        if (currentGap < MIN_COMBINING_GAP_PX) raiseBy = MIN_COMBINING_GAP_PX - currentGap;
      }
      const int combiningX = x - raiseBy;
      const int combiningY = lastBaseY - fp4::toPixel(lastBaseAdvanceFP / 2);
      renderCharImpl<TextRotation::Rotated90CW>(*this, font, cp, combiningX, combiningY,
                                                black ? Color::Black : Color::White, style);
      continue;
    }
    cp = font.applyLigatures(cp, text, style);
    if (prevCp != 0) yPosFP -= font.getKerning(prevCp, cp, style);
    lastBaseY = fp4::toPixel(yPosFP);
    const EpdGlyph* glyph = font.getGlyph(cp, style);
    lastBaseAdvanceFP = glyph ? glyph->advanceX : 0;
    lastBaseTop = glyph ? glyph->top : 0;
    renderCharImpl<TextRotation::Rotated90CW>(*this, font, cp, x, lastBaseY, black ? Color::Black : Color::White,
                                              style);
    if (glyph) yPosFP -= glyph->advanceX;
    prevCp = cp;
  }
}

void GfxRenderer::getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const {
  switch (orientation) {
    case Portrait:
      *outTop = VIEWABLE_MARGIN_TOP;
      *outRight = VIEWABLE_MARGIN_RIGHT;
      *outBottom = VIEWABLE_MARGIN_BOTTOM;
      *outLeft = VIEWABLE_MARGIN_LEFT;
      break;
    case LandscapeClockwise:
      *outTop = VIEWABLE_MARGIN_LEFT;
      *outRight = VIEWABLE_MARGIN_TOP;
      *outBottom = VIEWABLE_MARGIN_RIGHT;
      *outLeft = VIEWABLE_MARGIN_BOTTOM;
      break;
    case PortraitInverted:
      *outTop = VIEWABLE_MARGIN_BOTTOM;
      *outRight = VIEWABLE_MARGIN_LEFT;
      *outBottom = VIEWABLE_MARGIN_TOP;
      *outLeft = VIEWABLE_MARGIN_RIGHT;
      break;
    case LandscapeCounterClockwise:
      *outTop = VIEWABLE_MARGIN_RIGHT;
      *outRight = VIEWABLE_MARGIN_BOTTOM;
      *outBottom = VIEWABLE_MARGIN_LEFT;
      *outLeft = VIEWABLE_MARGIN_TOP;
      break;
  }
}
