#pragma once

#ifdef ENABLE_CUSTOM_FONTS

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class CustomFontActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  std::vector<std::string> files;
  std::string basepath = "/";
  std::string selectedFontPath;
  size_t selectorIndex = 0;
  bool previewMode = false;
  bool sizeSelectionMode = false;
  int targetSize = 14;

  void loadFiles();
  size_t findEntry(const std::string& name) const;
  void onSelectFile(const std::string& fullPath);

 public:
  explicit CustomFontActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("CustomFont", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

#endif
