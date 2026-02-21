#include "FrontlightControlActivity.h"

#ifdef FRONTLIGHT_PRESENT

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "FrontlightGlobal.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

FrontlightControlActivity::FrontlightControlActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     std::function<void()> onGoBack)
    : ActivityWithSubactivity("Frontlight", renderer, mappedInput), onGoBack(std::move(onGoBack)), buttonNavigator() {}

void FrontlightControlActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  skipNextButtonCheck = true;
  if (SETTINGS.frontlightEnabled && SETTINGS.frontlightBrightness > 0) {
    circuitPassed = true;
    applyLightSettings();
  } else {
    // If not enabled or brightness is 0, we require a test to enable
    SETTINGS.frontlightEnabled = false;
    circuitPassed = false;
  }
  requestUpdate();
}

void FrontlightControlActivity::onExit() { ActivityWithSubactivity::onExit(); }

void FrontlightControlActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (skipNextButtonCheck) {
    const bool confirmCleared = !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                                !mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    const bool backCleared = !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                             !mappedInput.wasReleased(MappedInputManager::Button::Back);
    if (confirmCleared && backCleared) {
      skipNextButtonCheck = false;
    }
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up1}, [this] {
    if (SETTINGS.frontlightBrightness >= 10 && SETTINGS.frontlightBrightness < 50) {
      SETTINGS.frontlightBrightness += 5;
    } else if (SETTINGS.frontlightBrightness < 10) {
      SETTINGS.frontlightBrightness += 1;
    }
    if (SETTINGS.frontlightBrightness > 50) SETTINGS.frontlightBrightness = 50;
    applyLightSettings();
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down1}, [this] {
    if (SETTINGS.frontlightBrightness > 10) {
      SETTINGS.frontlightBrightness -= 5;
    } else if (SETTINGS.frontlightBrightness > 0) {
      SETTINGS.frontlightBrightness -= 1;
    }
    applyLightSettings();
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up2}, [this] {
    if (SETTINGS.frontlightWarmth <= 95) {
      SETTINGS.frontlightWarmth += 5;
    } else {
      SETTINGS.frontlightWarmth = 100;
    }
    applyLightSettings();
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down2}, [this] {
    if (SETTINGS.frontlightWarmth >= 5) {
      SETTINGS.frontlightWarmth -= 5;
    } else {
      SETTINGS.frontlightWarmth = 0;
    }
    applyLightSettings();
    requestUpdate();
  });

  buttonNavigator.onRelease({MappedInputManager::Button::Confirm}, [this] {
    if (!SETTINGS.frontlightEnabled) {
      // Turning ON
      frontlightManager.setBrightness(SETTINGS.frontlightBrightness);
      frontlightManager.setColorTemperature(SETTINGS.frontlightWarmth);
      if (frontlightManager.enable()) {
        SETTINGS.frontlightEnabled = true;
        circuitPassed = true;
      } else {
        SETTINGS.frontlightEnabled = false;
        circuitPassed = false;
      }
    } else {
      // Turning OFF
      SETTINGS.frontlightEnabled = false;
      frontlightManager.disable();
    }
    applyLightSettings();
    requestUpdate();
  });

  buttonNavigator.onRelease({MappedInputManager::Button::Back}, [this] {
    SETTINGS.saveToFile();
    if (this->onGoBack) {
      this->onGoBack();
    }
  });
}

void FrontlightControlActivity::applyLightSettings() {
  if (SETTINGS.frontlightEnabled && circuitPassed) {
    frontlightManager.setBrightness(SETTINGS.frontlightBrightness);
    frontlightManager.setColorTemperature(SETTINGS.frontlightWarmth);
  } else {
    frontlightManager.setBrightness(0);
  }
}

void FrontlightControlActivity::render(RenderLock&&) {
  if (subActivity) {
    return;
  }

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const auto metrics = UITheme::getInstance().getMetrics();

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  const int popupWidth = screenWidth - (orientedMarginLeft + orientedMarginRight) - 40;
  const int popupHeight = 280;
  const int popupX = orientedMarginLeft + 20;
  const int popupY = (screenHeight - popupHeight) / 2;

  renderer.fillRect(popupX, popupY, popupWidth, popupHeight, false);    // White background block
  renderer.drawRect(popupX, popupY, popupWidth, popupHeight, 3, true);  // Thick black border

  const int centerX = popupX + popupWidth / 2;
  int currY = popupY + metrics.contentSidePadding;

  const std::string title = "Frontlight Control";
  renderer.drawCenteredText(UI_12_FONT_ID, currY, title.c_str(), true, EpdFontFamily::BOLD);
  currY += renderer.getLineHeight(UI_12_FONT_ID) + 10;

  std::string toggleStr =
      SETTINGS.frontlightEnabled ? "Status: ON (CONFIRM to turn OFF)" : "Status: OFF (CONFIRM to turn ON)";
  renderer.drawCenteredText(UI_10_FONT_ID, currY, toggleStr.c_str(), true);
  currY += renderer.getLineHeight(UI_10_FONT_ID) + 20;

  if (circuitPassed && SETTINGS.frontlightEnabled) {
    auto drawSliderRow = [&](const std::string& label, int value, int max_val, const char* hint) {
      renderer.drawCenteredText(UI_10_FONT_ID, currY, label.c_str(), true, EpdFontFamily::BOLD);
      currY += renderer.getLineHeight(UI_10_FONT_ID) + 5;

      const int barWidth = 300;
      const int barHeight = 12;
      const int barX = centerX - barWidth / 2;
      renderer.drawRect(barX, currY, barWidth, barHeight, 1, true);

      const int fillWidth = (barWidth * value) / max_val;
      if (fillWidth > 0) {
        renderer.fillRect(barX, currY, fillWidth, barHeight, true);
      }

      std::string valStr = std::to_string(value) + "%";
      const int valX = barX + barWidth + 10;
      renderer.drawText(UI_10_FONT_ID, valX, currY - 2, valStr.c_str(), true);

      currY += barHeight + 5;
      renderer.drawCenteredText(SMALL_FONT_ID, currY, hint, true);
      currY += renderer.getLineHeight(SMALL_FONT_ID) + 15;
    };

    drawSliderRow("Brightness", SETTINGS.frontlightBrightness, 50, "(Up1 / Down1)");
    drawSliderRow("Warmth", SETTINGS.frontlightWarmth, 100, "(Up2 / Down2)");
  } else if (!circuitPassed && SETTINGS.frontlightEnabled == false) {
    const std::string failMsg = "Frontlight not detected or disabled.";
    renderer.drawCenteredText(UI_10_FONT_ID, currY + 20, failMsg.c_str(), true);
  }

  const std::string hint = "Press BACK to save & exit.";
  renderer.drawCenteredText(SMALL_FONT_ID, popupY + popupHeight - 25, hint.c_str(), true);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

#endif  // FRONTLIGHT_PRESENT
