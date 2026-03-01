#include "CustomFontActivity.h"

#ifdef ENABLE_CUSTOM_FONTS

#include <algorithm>
#include <string>
#include <utility>

#include "CrossPointSettings.h"
#include "CustomFontManager.h"
#include "GfxRenderer.h"
#include "HalStorage.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    // Directories first
    bool isDir1 = str1.back() == '/';
    bool isDir2 = str2.back() == '/';
    if (isDir1 != isDir2) return isDir1;

    // Start naive natural sort
    const char* s1 = str1.c_str();
    const char* s2 = str2.c_str();

    // Iterate while both strings have characters
    while (*s1 && *s2) {
      if (isdigit(*s1) && isdigit(*s2)) {
        const char* start1 = s1;
        const char* start2 = s2;
        while (*s1 == '0') s1++;
        while (*s2 == '0') s2++;

        int len1 = 0, len2 = 0;
        while (isdigit(s1[len1])) len1++;
        while (isdigit(s2[len2])) len2++;

        if (len1 != len2) return len1 < len2;

        for (int i = 0; i < len1; i++) {
          if (s1[i] != s2[i]) return s1[i] < s2[i];
        }

        s1 += len1;
        s2 += len2;
      } else {
        char c1 = tolower(*s1);
        char c2 = tolower(*s2);
        if (c1 != c2) return c1 < c2;
        s1++;
        s2++;
      }
    }

    return *s1 == '\0' && *s2 != '\0';
  });
}

}  // namespace

void CustomFontActivity::loadFiles() {
  files.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/");
    } else {
      auto filename = std::string(name);
      if (StringUtils::checkFileExtension(filename, ".ttf") || StringUtils::checkFileExtension(filename, ".TTF")) {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);
}

void CustomFontActivity::onEnter() {
  Activity::onEnter();
  loadFiles();
  selectorIndex = 0;

  // Initialize targetSize based on current settings
  switch (SETTINGS.fontSize) {
    case CrossPointSettings::SMALL:
      targetSize = 12;
      break;
    case CrossPointSettings::MEDIUM:
      targetSize = 14;
      break;
    case CrossPointSettings::LARGE:
      targetSize = 16;
      break;
    case CrossPointSettings::EXTRA_LARGE:
      targetSize = 18;
      break;
    default:
      targetSize = 14;
      break;
  }

  requestUpdate();
}

void CustomFontActivity::onExit() {
  Activity::onExit();
  files.clear();
}

void CustomFontActivity::onSelectFile(const std::string& fullPath) {
  selectedFontPath = fullPath;
  sizeSelectionMode = true;
  previewMode = false;
  requestUpdate();
}

void CustomFontActivity::loop() {
  if (sizeSelectionMode) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      sizeSelectionMode = false;
      // Attempt to parse and allocate
      renderer.clearScreen();
      char msg[64];
      snprintf(msg, sizeof(msg), "Generating %dpt font...", targetSize);
      renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2 - 10, msg);
      renderer.displayBuffer();

      // IMPORTANT: Use RenderLock to ensure renderer doesn't use the font while we swap it
      {
        RenderLock lock;
        renderer.removeFont(CUSTOM_PSRAM_FONT_ID);
        bool success = CustomFontManager::setActiveCustomFont(String(selectedFontPath.c_str()), targetSize, true);

        if (success) {
          // Update renderer's reference
          renderer.insertFont(CUSTOM_PSRAM_FONT_ID, *CustomFontManager::getActiveCustomFont());
          previewMode = true;
          requestUpdate();
        } else {
          // Failed
          renderer.clearScreen();
          renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2 - 10, "Font Generation Failed");
          renderer.displayBuffer();
          delay(2000);
          sizeSelectionMode = true;  // Back to size selection
          requestUpdate();
        }
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      sizeSelectionMode = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      targetSize = std::min(32, targetSize + 2);
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      targetSize = std::max(8, targetSize - 2);
      requestUpdate();
      return;
    }
    return;
  }

  if (previewMode) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Automatically switch font family
      SETTINGS.fontFamily = CrossPointSettings::CUSTOM_FONT;
      SETTINGS.saveToFile();
      ActivityResult res;
      res.isCancelled = false;
      setResult(std::move(res));
      finish();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      sizeSelectionMode = true;  // Go back to size selection instead of browser
      previewMode = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      targetSize = std::min(32, targetSize + 2);
      // Re-trigger generation flow
      sizeSelectionMode = true;
      previewMode = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      targetSize = std::max(8, targetSize - 2);
      // Re-trigger generation flow
      sizeSelectionMode = true;
      previewMode = false;
      requestUpdate();
      return;
    }
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS &&
      basepath != "/") {
    basepath = "/";
    loadFiles();
    selectorIndex = 0;
    return;
  }

  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (files.empty()) return;

    const std::string& entry = files[selectorIndex];
    bool isDirectory = (entry.back() == '/');

    if (isDirectory) {
      std::string folderName = entry.substr(0, entry.length() - 1);
      std::string folderPath = basepath;
      if (folderPath.back() != '/') folderPath += "/";
      folderPath += folderName;

      // Try to find a _reg.ttf inside
      std::string regPath = folderPath + "/" + folderName + "_reg.ttf";
      if (Storage.exists(regPath.c_str())) {
        onSelectFile(regPath);
      } else {
        // Fallback: navigate inside
        basepath = folderPath;
        loadFiles();
        selectorIndex = 0;
        requestUpdate();
      }
    } else {
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      onSelectFile(cleanBasePath + entry);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;
        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();
        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findEntry(dirName);
        requestUpdate();
      } else {
        ActivityResult res;
        res.isCancelled = true;
        setResult(std::move(res));
        finish();
      }
    }
  }

  int listSize = static_cast<int>(files.size());
  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void CustomFontActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (sizeSelectionMode) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Select Font Size");

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;

    // Show font name
    std::string fontName = selectedFontPath.substr(selectedFontPath.find_last_of('/') + 1);
    renderer.drawCenteredText(UI_12_FONT_ID, contentTop + 20, fontName.c_str());

    // Show size with arrows
    char sizeText[32];
    snprintf(sizeText, sizeof(sizeText), "<  %d pt  >", targetSize);
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 80, sizeText);

    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 130, "Use Up/Down to adjust size");

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (previewMode) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Font Preview");

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
    int y = contentTop;
    int spacing = targetSize + 14;

    renderer.drawText(CUSTOM_PSRAM_FONT_ID, metrics.contentSidePadding, y, "Regular: The quick fox jumps...", true,
                      EpdFontFamily::REGULAR);
    y += spacing;
    renderer.drawText(CUSTOM_PSRAM_FONT_ID, metrics.contentSidePadding, y, "Bold: The quick fox jumps...", true,
                      EpdFontFamily::BOLD);
    y += spacing;
    renderer.drawText(CUSTOM_PSRAM_FONT_ID, metrics.contentSidePadding, y, "Italic: The quick fox jumps...", true,
                      EpdFontFamily::ITALIC);
    y += spacing;
    renderer.drawText(CUSTOM_PSRAM_FONT_ID, metrics.contentSidePadding, y, "BoldItalic: The quick fox jumps...", true,
                      EpdFontFamily::BOLD_ITALIC);

    // RAM usage display
    char ramText[64];
    size_t ramBytes = CustomFontManager::getTotalRamUsage();
    snprintf(ramText, sizeof(ramText), "Size: %dpt | RAM: %.1f KB", targetSize, ramBytes / 1024.0);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, pageHeight - metrics.buttonHintsHeight - 20, ramText);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "Size -", "Size +");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  std::string folderName = (basepath == "/") ? tr(STR_SD_CARD) : basepath.substr(basepath.rfind('/') + 1);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, "No TTF fonts found");
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, files.size(), selectorIndex,
        [this](int index) {
          std::string name = files[index];
          if (name.back() == '/') return name.substr(0, name.length() - 1);
          return name;
        },
        nullptr, [this](int index) { return UITheme::getFileIcon(files[index]); });
  }

  const auto labels = mappedInput.mapLabels(basepath == "/" ? tr(STR_BACK) : tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

size_t CustomFontActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}

#endif
