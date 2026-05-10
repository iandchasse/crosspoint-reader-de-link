#pragma once

#include <Arduino.h>

// TODO: Move enums into new header and share with CrossPointSettings.h
namespace CrossPointOrientation {
enum Value : uint8_t { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3 };
}

namespace CrossPointTiltPageTurn {
enum Value : uint8_t { TILT_OFF = 0, TILT_NORMAL = 1, TILT_INVERTED = 2 };
}

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;  // Singleton

/// Stub for S3 platform — no QMI8658 tilt sensor hardware present.
/// All methods are no-ops. The class and singleton are kept so upstream
/// code that references tilt settings and halTiltSensor compiles cleanly.
class HalTiltSensor {
  bool _available = false;

 public:
  void begin();
  bool wake();
  bool deepSleep();
  bool isAvailable() const { return _available; }
  void update(const uint8_t mode, const uint8_t orientation, const bool inReader);
  bool wasTiltedForward();
  bool wasTiltedBack();
  bool hadActivity();
  void clearPendingEvents();
};
