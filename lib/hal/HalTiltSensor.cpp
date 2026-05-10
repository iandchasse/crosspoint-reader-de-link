#include "HalTiltSensor.h"

// S3 platform: No QMI8658 tilt sensor hardware present.
// All methods are stubbed to no-op. The singleton is still instantiated
// so that upstream code referencing halTiltSensor compiles and links.

HalTiltSensor halTiltSensor;  // Singleton instance

void HalTiltSensor::begin() { _available = false; }
bool HalTiltSensor::wake() { return false; }
bool HalTiltSensor::deepSleep() { return false; }
void HalTiltSensor::update(const uint8_t, const uint8_t, const bool) {}
bool HalTiltSensor::wasTiltedForward() { return false; }
bool HalTiltSensor::wasTiltedBack() { return false; }
bool HalTiltSensor::hadActivity() { return false; }
void HalTiltSensor::clearPendingEvents() {}
