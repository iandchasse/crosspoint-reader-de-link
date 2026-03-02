#pragma once

#include <HalStorage.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Forward declarations
class HttpDownloader;

class FontDownloadActivity : public Activity {
 public:
  enum State { INIT = 0, CHOOSE_TYPE, CHECKING_FOR_FONTS, SELECT_FONTS, DOWNLOADING, FAILED, FINISHED };

  FontDownloadActivity(GfxRenderer& r, MappedInputManager& input) : Activity("Font Download", r, input), state(INIT) {}

  void onWifiSelectionComplete(const bool success);

 protected:
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  struct FontFile {
    std::string name;
    std::string downloadUrl;
    size_t size;
  };

  struct FontFamily {
    std::string name;
    std::string url;
    std::vector<FontFile> files;
  };

  bool fetchFontFamilies();
  bool fetchFontFiles(FontFamily& family);
  bool downloadFontFiles();

  State state;
  ButtonNavigator buttonNavigator;
  std::string errorMessage;

  std::vector<FontFamily> fontFamilies;
  std::vector<bool> selectedFamilies;
  int scrollOffset = 0;
  int selectedIndex = 0;
  bool preconvertedType = true;

  size_t currentFamilyIndex = 0;
  size_t currentFileIndex = 0;

  // Downloading progress
  size_t totalBytesDownloaded = 0;
  size_t totalBytesOverall = 0;
  size_t currentFileDownloaded = 0;
  size_t currentFileTotal = 0;
  int lastDlPercentage = -1;
};
