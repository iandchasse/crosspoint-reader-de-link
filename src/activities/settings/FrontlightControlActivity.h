#pragma once

#ifdef FRONTLIGHT_PRESENT

#include <functional>

#include "activities/ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

// Forward declaration
class GfxRenderer;
class MappedInputManager;

class FrontlightControlActivity : public ActivityWithSubactivity {
 public:
  explicit FrontlightControlActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     std::function<void()> onGoBack);
  ~FrontlightControlActivity() override = default;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::function<void()> onGoBack;
  ButtonNavigator buttonNavigator;

  void applyLightSettings();

  bool circuitPassed = true;
  bool skipNextButtonCheck = true;
};

#endif  // FRONTLIGHT_PRESENT
