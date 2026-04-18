#include "ScreenshotUtil.h"

#include <Arduino.h>
#include <BitmapHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <time.h>

#include <string>

#include "Bitmap.h"  // Required for BmpHeader struct definition
#include "HalClock.h"

void ScreenshotUtil::takeScreenshot(GfxRenderer& renderer) {
  const uint8_t* fb = renderer.getFrameBuffer();
  if (fb) {
    String filename_str;
    if (HalClock::isSynced()) {
      time_t now;
      ::time(&now);
      struct tm timeinfo;
      localtime_r(&now, &timeinfo);
      char buf[32];
      // MM_DD_YYYY_HH_mm format for filename compatibility
      snprintf(buf, sizeof(buf), "%02d_%02d_%04d_%02d_%02d", timeinfo.tm_mon + 1, timeinfo.tm_mday,
               timeinfo.tm_year + 1900, timeinfo.tm_hour, timeinfo.tm_min);
      filename_str = "/screenshots/screenshot-" + String(buf) + ".bmp";
    } else {
      filename_str = "/screenshots/screenshot-" + String(millis()) + ".bmp";
    }
    if (ScreenshotUtil::saveFramebufferAsBmp(filename_str.c_str(), fb, HalDisplay::DISPLAY_WIDTH,
                                             HalDisplay::DISPLAY_HEIGHT)) {
      LOG_DBG("SCR", "Screenshot saved to %s", filename_str.c_str());
    } else {
      LOG_ERR("SCR", "Failed to save screenshot");
    }
  } else {
    LOG_ERR("SCR", "Framebuffer not available");
  }

  // Display a border around the screen to indicate a screenshot was taken
  if (renderer.storeBwBuffer()) {
    renderer.drawRect(6, 6, HalDisplay::DISPLAY_HEIGHT - 12, HalDisplay::DISPLAY_WIDTH - 12, 2, true);
    renderer.displayBuffer();
    delay(1000);
    renderer.restoreBwBuffer();
    renderer.displayBuffer(HalDisplay::RefreshMode::HALF_REFRESH);
  }
}

bool ScreenshotUtil::saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width, int height) {
  if (!framebuffer) {
    return false;
  }

  // Note: the width and height, we rotate the image 90d counter-clockwise to match the default display orientation
  int phyWidth = height;
  int phyHeight = width;

  std::string path(filename);
  size_t last_slash = path.find_last_of('/');
  if (last_slash != std::string::npos) {
    std::string dir = path.substr(0, last_slash);
    if (!Storage.exists(dir.c_str())) {
      if (!Storage.mkdir(dir.c_str())) {
        return false;
      }
    }
  }

  EspFsFile file;
  if (!Storage.openFileForWrite("SCR", filename, file)) {
    LOG_ERR("SCR", "Failed to save screenshot");
    return false;
  }

  BmpHeader header;

  createBmpHeader(&header, phyWidth, phyHeight, BmpRowOrder::BottomUp);

  bool write_error = false;
  if (file.write(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
    write_error = true;
  }

  if (write_error) {
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filename);
    return false;
  }

  const uint32_t rowSizePadded = (phyWidth + 31) / 32 * 4;
  // Max row size for 480px width = 60 bytes; use fixed buffer to avoid VLA
  constexpr size_t kMaxRowSize = 64;
  if (rowSizePadded > kMaxRowSize) {
    LOG_ERR("SCR", "Row size %u exceeds buffer capacity", rowSizePadded);
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filename);
    return false;
  }

  // rotate the image 90d counter-clockwise on-the-fly while writing to save memory
  uint8_t rowBuffer[kMaxRowSize];
  memset(rowBuffer, 0, rowSizePadded);

  for (int outY = 0; outY < phyHeight; outY++) {
    for (int outX = 0; outX < phyWidth; outX++) {
      // 270d counter-clockwise (90d clockwise): source (srcX, srcY)
      // BMP rows are bottom-to-top, so outY=0 is the bottom of the displayed image
      int srcX = outY;
      int srcY = outX;

      /* Original 90d counter-clockwise logic:
      int srcX = width - 1 - outY;     // phyHeight == width
      int srcY = phyWidth - 1 - outX;  // phyWidth == height
      */

      int fbIndex = srcY * (width / 8) + (srcX / 8);
      uint8_t pixel = (framebuffer[fbIndex] >> (7 - (srcX % 8))) & 0x01;
      rowBuffer[outX / 8] |= pixel << (7 - (outX % 8));
    }
    if (file.write(rowBuffer, rowSizePadded) != rowSizePadded) {
      write_error = true;
      break;
    }
    memset(rowBuffer, 0, rowSizePadded);  // Clear the buffer for the next row
  }

  // Explicitly close() file before calling Storage.remove()
  file.close();

  if (write_error) {
    Storage.remove(filename);
    return false;
  }

  return true;
}
