#pragma once

#ifdef FRONTLIGHT_PRESENT

#include <functional>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Forward declaration
class GfxRenderer;
class MappedInputManager;

class FrontlightControlActivity : public Activity {
 public:
  explicit FrontlightControlActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~FrontlightControlActivity() override = default;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  void applyLightSettings();

  bool circuitPassed = true;
  bool skipNextButtonCheck = true;
};

#endif  // FRONTLIGHT_PRESENT
