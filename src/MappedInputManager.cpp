#include "MappedInputManager.h"

#include "CrossPointSettings.h"

namespace {
using ButtonIndex = uint8_t;

struct SideLayoutMap {
  ButtonIndex pageBack;
  ButtonIndex pageForward;
};

// Order matches CrossPointSettings::SIDE_BUTTON_LAYOUT.
constexpr SideLayoutMap kSideLayouts[] = {
    {HalGPIO::BTN_UP, HalGPIO::BTN_DOWN},
    {HalGPIO::BTN_DOWN, HalGPIO::BTN_UP},
};
}  // namespace

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);
  const auto& side = kSideLayouts[sideLayout];

  // Helper lambda to flip front buttons if the device is held upside down physically
  // Native orientation (0 deg/CCW) and PortraitInverted (270 deg) mean the buttons are on the right/bottom
  // Portrait (90 deg) and CCW (180 deg) mean the buttons are physically rotated 180 degrees.
  auto getPhysicalButton = [&](uint8_t logicalHwBtn) -> uint8_t {
    const bool isFlipped = (SETTINGS.orientation == CrossPointSettings::PORTRAIT ||
                            SETTINGS.orientation == CrossPointSettings::LANDSCAPE_CW);
    if (!isFlipped) return logicalHwBtn;

    switch (logicalHwBtn) {
      case HalGPIO::BTN_BACK:
        return HalGPIO::BTN_RIGHT;
      case HalGPIO::BTN_CONFIRM:
        return HalGPIO::BTN_LEFT;
      case HalGPIO::BTN_LEFT:
        return HalGPIO::BTN_CONFIRM;
      case HalGPIO::BTN_RIGHT:
        return HalGPIO::BTN_BACK;
      default:
        return logicalHwBtn;
    }
  };

  switch (button) {
    case Button::Back:
      return (gpio.*fn)(getPhysicalButton(SETTINGS.frontButtonBack));
    case Button::Confirm:
      return (gpio.*fn)(getPhysicalButton(SETTINGS.frontButtonConfirm));
    case Button::Left:
      return (gpio.*fn)(getPhysicalButton(SETTINGS.frontButtonLeft));
    case Button::Right:
      return (gpio.*fn)(getPhysicalButton(SETTINGS.frontButtonRight));
    case Button::Up:
      // Both side-button combos trigger Up (BTN_UP = combo 1, BTN_UNKNOWN_1 = combo 2).
      return (gpio.*fn)(HalGPIO::BTN_UP) || (gpio.*fn)(HalGPIO::BTN_UNKNOWN_1);
    case Button::Down:
      // Both side-button combos trigger Down (BTN_DOWN = combo 1, BTN_UNKNOWN_2 = combo 2).
      return (gpio.*fn)(HalGPIO::BTN_DOWN) || (gpio.*fn)(HalGPIO::BTN_UNKNOWN_2);
    case Button::Up1:
      // Specifically side combo 1 up
      return (gpio.*fn)(side.pageBack == HalGPIO::BTN_UP ? side.pageBack : side.pageForward);
    case Button::Down1:
      // Specifically side combo 1 down
      return (gpio.*fn)(side.pageBack == HalGPIO::BTN_DOWN ? side.pageBack : side.pageForward);
    case Button::Up2:
      // Specifically side combo 2 up
      return (gpio.*fn)(HalGPIO::BTN_UNKNOWN_1);
    case Button::Down2:
      // Specifically side combo 2 down
      return (gpio.*fn)(HalGPIO::BTN_UNKNOWN_2);
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack: {
      // Reader page navigation uses side buttons and can be swapped via settings.
      // BTN_UP/BTN_DOWN use side combo 1, BTN_UNKNOWN_1/BTN_UNKNOWN_2 use side combo 2.
      const bool combo1 = (gpio.*fn)(side.pageBack);
      const uint8_t combo2Hw = (side.pageBack == HalGPIO::BTN_UP) ? HalGPIO::BTN_UNKNOWN_1 : HalGPIO::BTN_UNKNOWN_2;
      return combo1 || (gpio.*fn)(combo2Hw);
    }
    case Button::PageForward: {
      // Reader page navigation uses side buttons and can be swapped via settings.
      const bool combo1 = (gpio.*fn)(side.pageForward);
      const uint8_t combo2Hw = (side.pageForward == HalGPIO::BTN_UP) ? HalGPIO::BTN_UNKNOWN_1 : HalGPIO::BTN_UNKNOWN_2;
      return combo1 || (gpio.*fn)(combo2Hw);
    }
  }

  return false;
}

bool MappedInputManager::wasPressed(const Button button) const { return mapButton(button, &HalGPIO::wasPressed); }

bool MappedInputManager::wasReleased(const Button button) const { return mapButton(button, &HalGPIO::wasReleased); }

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    // Helper lambda to flip front buttons if the device is held upside down physically
    auto getPhysicalButton = [&](uint8_t logicalHwBtn) -> uint8_t {
      const bool isFlipped = (SETTINGS.orientation == CrossPointSettings::PORTRAIT ||
                              SETTINGS.orientation == CrossPointSettings::LANDSCAPE_CW);
      if (!isFlipped) return logicalHwBtn;

      switch (logicalHwBtn) {
        case HalGPIO::BTN_BACK:
          return HalGPIO::BTN_RIGHT;
        case HalGPIO::BTN_CONFIRM:
          return HalGPIO::BTN_LEFT;
        case HalGPIO::BTN_LEFT:
          return HalGPIO::BTN_CONFIRM;
        case HalGPIO::BTN_RIGHT:
          return HalGPIO::BTN_BACK;
        default:
          return logicalHwBtn;
      }
    };

    if (hw == getPhysicalButton(SETTINGS.frontButtonBack)) {
      return back;
    }
    if (hw == getPhysicalButton(SETTINGS.frontButtonConfirm)) {
      return confirm;
    }
    if (hw == getPhysicalButton(SETTINGS.frontButtonLeft)) {
      return previous;
    }
    if (hw == getPhysicalButton(SETTINGS.frontButtonRight)) {
      return next;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}