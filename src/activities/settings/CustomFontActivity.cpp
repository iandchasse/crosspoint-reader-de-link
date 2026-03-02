#include "CustomFontActivity.h"

#ifdef ENABLE_CUSTOM_FONTS

#include <ArduinoJson.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "CrossPointSettings.h"
#include "EpdFontFileLoader.h"
#include "EpdFontSerializer.h"
#include "GfxRenderer.h"
#include "HalStorage.h"
#include "I18n.h"
#include "Logging.h"
#include "MappedInputManager.h"
#include "RuntimeFontConverter.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

// ─── anonymous helpers ────────────────────────────────────────────────────────

namespace {
constexpr unsigned long GO_HOME_MS = 1000;

// Recursive directory delete
static void deleteDirectory(const char* dir) {
  auto root = Storage.open(dir);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    Storage.remove(dir);
    return;
  }
  root.rewindDirectory();
  char name[256];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.') {
      file.close();
      continue;
    }
    std::string fullPath = std::string(dir) + "/" + name;
    if (file.isDirectory()) {
      file.close();
      deleteDirectory(fullPath.c_str());
    } else {
      file.close();
      Storage.remove(fullPath.c_str());
    }
  }
  root.close();
  Storage.remove(dir);
}

// Check whether /.fonts/<family>/ already contains at least one .epdfont
static bool cacheExists(const std::string& dotFontsDir) {
  auto root = Storage.open(dotFontsDir.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return false;
  }
  root.rewindDirectory();
  char name[256];
  bool found = false;
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    file.close();
    std::string n(name);
    if (n.size() > 8 && n.substr(n.size() - 8) == ".epdfont") {
      found = true;
      break;
    }
  }
  root.close();
  return found;
}

// Case-insensitive "ends with" check for filenames
static bool endsWithCI(const std::string& s, const std::string& suffix) {
  if (s.size() < suffix.size()) return false;
  std::string tail = s.substr(s.size() - suffix.size());
  std::transform(tail.begin(), tail.end(), tail.begin(), ::tolower);
  std::string sfxLower = suffix;
  std::transform(sfxLower.begin(), sfxLower.end(), sfxLower.begin(), ::tolower);
  return tail == sfxLower;
}

static bool copyFile(const std::string& src, const std::string& dst) {
  EspFsFile s, d;
  if (!Storage.openFileForRead("COPY", src, s)) return false;
  if (!Storage.openFileForWrite("COPY", dst, d)) {
    s.close();
    return false;
  }
  uint8_t buf[1024];
  int n;
  while ((n = s.read(buf, sizeof(buf))) > 0) {
    if (d.write(buf, n) != n) {
      s.close();
      d.close();
      return false;
    }
  }
  s.close();
  d.close();
  return true;
}

static void copyDirectory(const std::string& src, const std::string& dst) {
  Storage.mkdir(dst.c_str());
  auto root = Storage.open(src.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }
  char name[256];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    std::string s = src + "/" + name;
    std::string d = dst + "/" + name;
    if (file.isDirectory()) {
      file.close();
      copyDirectory(s, d);
    } else {
      file.close();
      copyFile(s, d);
    }
  }
  root.close();
}

// Alphabetical sort for FolderInfo
static void sortList(std::vector<CustomFontActivity::FolderInfo>& folders) {
  std::sort(
      folders.begin(), folders.end(),
      [](const CustomFontActivity::FolderInfo& a, const CustomFontActivity::FolderInfo& b) { return a.name < b.name; });
}

static const char* SIZE_NAMES[EpdFontFileLoader::SIZE_SLOT_COUNT] = {"Small", "Medium", "Large", "XLarge"};
// STYLE_SUFFIXES used for both scanning and output file naming
constexpr const char* STYLE_SUFFIXES[] = {"regular", "bold", "italic", "bolditalic"};
constexpr int NUM_STYLES = 4;

}  // namespace

// ─── lifecycle ───────────────────────────────────────────────────────────────

void CustomFontActivity::onEnter() {
  Activity::onEnter();
  enterTimeMs = millis();
  basepath = "/fonts";
  state = State::BROWSER;
  selectorIndex = 0;
  sizeConfigRow = 0;
  foundStyleCount = 0;
  loadFolders();
  requestUpdate();
}

void CustomFontActivity::onExit() {
  Activity::onExit();
  cleanupPreview();
  folders.clear();
}

bool CustomFontActivity::preventAutoSleep() { return state == State::GENERATING; }
bool CustomFontActivity::skipLoopDelay() { return state == State::GENERATING; }

// ─── loadFolders ─────────────────────────────────────────────────────────────
// Lists only sub-directories of /fonts — each is a font family candidate.

void CustomFontActivity::loadFolders() {
  folders.clear();
  selectorIndex = 0;

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }
  root.rewindDirectory();
  char name[256];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    bool skip = (name[0] == '.' || strcmp(name, "System Volume Information") == 0);
    bool isDir = file.isDirectory();
    file.close();
    if (skip) continue;
    if (isDir) {
      folders.push_back(checkFolderType(name));
    }
  }
  root.close();
  sortList(folders);
}

CustomFontActivity::FolderInfo CustomFontActivity::checkFolderType(const std::string& name) {
  FolderInfo info{name, false, false};
  std::string path = basepath + "/" + name;
  auto dir = Storage.open(path.c_str());
  if (!dir) return info;

  dir.rewindDirectory();
  char fname[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(fname, sizeof(fname));
    file.close();
    if (fname[0] == '.') continue;

    std::string n(fname);
    if (endsWithCI(n, ".ttf") || endsWithCI(n, ".otf")) info.hasTtf = true;
    if (endsWithCI(n, ".epdfont")) info.hasEpd = true;

    if (info.hasTtf && info.hasEpd) break;
  }
  dir.close();
  return info;
}

// ─── scanFamilyStyles ────────────────────────────────────────────────────────
// Scans familyDir for TTF files ending in each style suffix (case-insensitive).
// Populates resolvedTtfPaths[] — empty string = not found (will fall back to regular).
// Returns true if at least the "regular" style was found.

bool CustomFontActivity::scanFamilyStyles(const std::string& familyDir) {
  // Reset
  for (int i = 0; i < NUM_STYLES; ++i) resolvedTtfPaths[i] = "";
  foundStyleCount = 0;

  // Collect all TTF files in the directory
  std::vector<std::string> ttfFiles;
  auto root = Storage.open(familyDir.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return false;
  }
  root.rewindDirectory();
  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    bool isDir = file.isDirectory();
    file.close();
    if (isDir) continue;
    std::string fname(name);
    if (endsWithCI(fname, ".ttf")) ttfFiles.push_back(fname);
  }
  root.close();

  // For each style suffix, find the first file whose stem (before .ttf) ends with that suffix (CI)
  for (int i = 0; i < NUM_STYLES; ++i) {
    std::string suffix(STYLE_SUFFIXES[i]);
    for (const auto& f : ttfFiles) {
      // Strip .ttf extension
      std::string stem = f.substr(0, f.size() - 4);
      std::string stemLower = stem;
      std::transform(stemLower.begin(), stemLower.end(), stemLower.begin(), ::tolower);
      if (stemLower.size() >= suffix.size() && stemLower.substr(stemLower.size() - suffix.size()) == suffix) {
        resolvedTtfPaths[i] = familyDir + "/" + f;
        foundStyleCount++;
        break;
      }
    }
  }

  // If "regular" (index 0) not found but we have any file at all, use the first TTF as fallback
  if (resolvedTtfPaths[0].empty() && !ttfFiles.empty()) {
    resolvedTtfPaths[0] = familyDir + "/" + ttfFiles[0];
    foundStyleCount++;
  }

  return !resolvedTtfPaths[0].empty();
}

size_t CustomFontActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < folders.size(); ++i)
    if (folders[i].name == name) return i;
  return 0;
}

// ─── onSelectFolder ──────────────────────────────────────────────────────────

void CustomFontActivity::onSelectFolder(const std::string& familyDir) {
  selectedFamilyPath = familyDir;
  std::string familyName = familyDir.substr(familyDir.rfind('/') + 1);
  std::string dotFontsDir = std::string("/.fonts/") + familyName;

  const FolderInfo& info = folders[selectorIndex];
  isImportMode = info.hasEpd && !cacheExists(dotFontsDir);

  if (info.hasEpd) {
    state = State::CONFIRM;
    sizeConfigRow = 0;
    requestUpdate();
    return;
  }

  // Scan for TTF files if not in import mode
  bool ok = scanFamilyStyles(familyDir);
  if (!ok) {
    genError = "No usable font files found in " + familyName;
    state = State::DONE_ERROR;
    sizeConfigRow = 0;
    requestUpdate();
    return;
  }

  if (cacheExists(dotFontsDir)) {
    state = State::CONFIRM;
    sizeConfigRow = 0;
  } else {
    state = State::SIZE_CONFIG;
    sizeConfigRow = 0;
    customPt[0] = 12;
    customPt[1] = 14;
    customPt[2] = 16;
    customPt[3] = 18;
    updatePreview(0);
  }
  requestUpdate();
}

// ─── updatePreview / cleanupPreview ──────────────────────────────────────────

void CustomFontActivity::cleanupPreview() {
  if (renderer.hasFont(PREVIEW_FONT_ID)) {
    renderer.removeFont(PREVIEW_FONT_ID);
  }
  if (previewFamily) {
    delete previewFamily;
    previewFamily = nullptr;
  }
  if (previewFont) {
    RuntimeFontConverter::freeEpdFont(previewFont);
    previewFont = nullptr;
  }
  lastPreviewSlot = -1;
}

void CustomFontActivity::updatePreview(int slot) {
  if (slot < 0 || slot >= NUM_SIZE_SLOTS) return;
  // Prevent redundant regenerations
  if (previewFont && lastPreviewedPt[slot] == customPt[slot] && lastPreviewSlot == slot) return;

  // Render a quick generating popup (does NOT delay main loop or anything intensive, just visual feedback)
  GUI.drawPopup(renderer, "Generating Preview...");
  renderer.displayBuffer();

  cleanupPreview();

  const std::string& ttfPath = resolvedTtfPaths[0].empty() ? "" : resolvedTtfPaths[0];
  if (ttfPath.empty()) return;

  const char* PREVIEW_PATH = "/.fonts/preview.epdfont";
  const char* PREVIEW_CHARSET = "The quick brown fox jumps over the lazy dog 0123456789";

  bool ok = RuntimeFontConverter::generateAndSaveToFile(ttfPath.c_str(), customPt[slot], SETTINGS.textAntiAliasing,
                                                        PREVIEW_PATH, PREVIEW_CHARSET);

  if (ok) {
    previewFont = EpdFontSerializer::loadFromFile(PREVIEW_PATH);
    if (previewFont) {
      previewFamily = new EpdFontFamily(previewFont);
      renderer.insertFont(PREVIEW_FONT_ID, *previewFamily);
    }
  }

  lastPreviewedPt[slot] = customPt[slot];
  lastPreviewSlot = slot;
}

// ─── startGeneration ─────────────────────────────────────────────────────────

void CustomFontActivity::startGeneration() {
  cleanupPreview();

  std::string familyName = selectedFamilyPath.substr(selectedFamilyPath.rfind('/') + 1);
  std::string dotFontsDir = std::string("/.fonts/") + familyName;
  deleteDirectory(dotFontsDir.c_str());

  state = State::GENERATING;
  genStep = 0;
  totalSteps = NUM_SIZE_SLOTS * NUM_STYLES;
  genOk = true;
  genError = "";
  // Acquire power lock — keeps CPU at full speed for the duration of generation
  pwrLock = std::make_unique<HalPowerManager::Lock>();
  // Small delay for SD card stability after potential deletions
  delay(100);
  requestUpdate();
}

// ─── loop ────────────────────────────────────────────────────────────────────

void CustomFontActivity::loop() {
  // Global input debounce on entry
  if (millis() - enterTimeMs < INPUT_DEBOUNCE_MS) return;

  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

  // ── BROWSER ──────────────────────────────────────────────────────────────
  if (state == State::BROWSER) {
    if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS) {
      finish();
      return;
    }

    buttonNavigator.onPreviousRelease([this] {
      selectorIndex = ButtonNavigator::previousIndex((int)selectorIndex, (int)folders.size());
      requestUpdate();
    });
    buttonNavigator.onNextRelease([this] {
      selectorIndex = ButtonNavigator::nextIndex((int)selectorIndex, (int)folders.size());
      requestUpdate();
    });
    buttonNavigator.onPreviousContinuous([this, pageItems] {
      selectorIndex = ButtonNavigator::previousPageIndex((int)selectorIndex, (int)folders.size(), pageItems);
      requestUpdate();
    });
    buttonNavigator.onNextContinuous([this, pageItems] {
      selectorIndex = ButtonNavigator::nextPageIndex((int)selectorIndex, (int)folders.size(), pageItems);
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < GO_HOME_MS) {
      finish();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!folders.empty()) {
        const auto& info = folders[selectorIndex];
        std::string fullPath = basepath;
        if (fullPath.back() != '/') fullPath += "/";
        fullPath += info.name;
        onSelectFolder(fullPath);
      }
    }
    return;
  }

  // ── CONFIRM (existing cache) ──────────────────────────────────────────────
  if (state == State::CONFIRM) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = State::BROWSER;
      requestUpdate();
      return;
    }
    buttonNavigator.onPreviousRelease([this] {
      if (sizeConfigRow > 0) {
        sizeConfigRow--;
        requestUpdate();
      }
    });
    buttonNavigator.onNextRelease([this] {
      if (sizeConfigRow < 3) {
        sizeConfigRow++;
        requestUpdate();
      }
    });
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      const auto& info = folders[selectorIndex];
      if (isImportMode) {
        if (sizeConfigRow == 0) {
          // [Import]
          state = State::GENERATING;
          genStep = 0;
        } else {
          // [Cancel]
          state = State::BROWSER;
          sizeConfigRow = 0;
        }
        requestUpdate();
      } else if (sizeConfigRow == 0) {
        // [Select]
        std::string familyName = selectedFamilyPath.substr(selectedFamilyPath.rfind('/') + 1);
        std::string dotFontsDir = std::string("/.fonts/") + familyName;

        // Try to load point sizes from config.json if it's the current family
        int pts[NUM_SIZE_SLOTS] = {12, 14, 16, 18};
        if (Storage.exists("/.fonts/config.json")) {
          String json = Storage.readFile("/.fonts/config.json");
          JsonDocument doc;
          if (!deserializeJson(doc, json)) {
            JsonArray ptsArr = doc["pts"];
            if (ptsArr.size() == NUM_SIZE_SLOTS) {
              for (int i = 0; i < NUM_SIZE_SLOTS; i++) pts[i] = ptsArr[i];
            }
          }
        }

        EpdFontFileLoader::setFamily(dotFontsDir.c_str(), pts);
        SETTINGS.fontFamily = CrossPointSettings::CUSTOM_FONT;
        SETTINGS.saveToFile();
        finish();
      } else if (sizeConfigRow == 1) {
        // [Regenerate] or [Re-import]
        if (info.hasEpd) {
          isImportMode = true;
          state = State::GENERATING;
          genStep = 0;
        } else {
          state = State::SIZE_CONFIG;
          customPt[0] = 12;
          customPt[1] = 14;
          customPt[2] = 16;
          customPt[3] = 18;
        }
        sizeConfigRow = 0;
        updatePreview(0);
        requestUpdate();
      } else if (sizeConfigRow == 2) {
        // [Delete cache]
        std::string familyName = selectedFamilyPath.substr(selectedFamilyPath.rfind('/') + 1);
        std::string dotFontsDir = std::string("/.fonts/") + familyName;
        deleteDirectory(dotFontsDir.c_str());
        if (EpdFontFileLoader::getFamilyName() == familyName.c_str()) {
          EpdFontFileLoader::clearFamily();
          if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FONT) {
            SETTINGS.fontFamily = CrossPointSettings::BOOKERLY;
            SETTINGS.saveToFile();
          }
        }
        genError = "Cache deleted for: " + familyName;
        state = State::DONE_OK;
        sizeConfigRow = 0;
        requestUpdate();
      } else {
        // [Back]
        state = State::BROWSER;
        requestUpdate();
      }
    }
    return;
  }

  // ── SIZE_CONFIG ───────────────────────────────────────────────────────────
  // Up/Down = move between size slots; Left/Right = decrease/increase pt value.
  // Confirm = advance to next row, or start generation if on Generate button.
  // ButtonNavigator is NOT used here to avoid PgFwd/PgBack conflicts.
  if (state == State::SIZE_CONFIG) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = State::BROWSER;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (sizeConfigRow > 0) {
        sizeConfigRow--;
        requestUpdate();
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      if (sizeConfigRow <= NUM_SIZE_SLOTS) {
        sizeConfigRow++;
        requestUpdate();
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (sizeConfigRow < NUM_SIZE_SLOTS) {
        customPt[sizeConfigRow] = std::max(6, customPt[sizeConfigRow] - 1);
        requestUpdate();
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (sizeConfigRow < NUM_SIZE_SLOTS) {
        customPt[sizeConfigRow] = std::min(36, customPt[sizeConfigRow] + 1);
        requestUpdate();
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (sizeConfigRow >= NUM_SIZE_SLOTS) {
        startGeneration();
      } else {
        updatePreview(sizeConfigRow);
        requestUpdate();
      }
    }
    return;
  }

  // ── GENERATING ───────────────────────────────────────────────────────────
  if (state == State::GENERATING) {
    if (isImportMode) {
      std::string familyName = selectedFamilyPath.substr(selectedFamilyPath.rfind('/') + 1);
      std::string dotFontsDir = std::string("/.fonts/") + familyName;
      Storage.mkdir("/.fonts");
      copyDirectory(selectedFamilyPath, dotFontsDir);

      // Wire up the new font family
      // Default pts for imported fonts; though we don't know exactly what's inside,
      // 12, 14, 16, 18 is a safe bet for the loader to at least try.
      int pts[NUM_SIZE_SLOTS] = {12, 14, 16, 18};
      String dotFontsBase = String("/.fonts/") + familyName.c_str();
      EpdFontFileLoader::setFamily(dotFontsBase, pts);
      SETTINGS.fontFamily = CrossPointSettings::CUSTOM_FONT;
      SETTINGS.saveToFile();
      EpdFontFileLoader::clearCache();

      state = State::DONE_OK;
      requestUpdate();
      return;
    }

    if (genStep < totalSteps) {
      int slot = genStep / NUM_STYLES;
      int styleIdx = genStep % NUM_STYLES;
      const char* styleSuffix = STYLE_SUFFIXES[styleIdx];

      // Use the pre-resolved TTF path; fall back to regular (index 0)
      const std::string& ttfPath =
          resolvedTtfPaths[styleIdx].empty() ? resolvedTtfPaths[0] : resolvedTtfPaths[styleIdx];

      if (resolvedTtfPaths[styleIdx].empty() && styleIdx != 0) {
        // Skip generating redundant _bold/_italic fallback files, the loader will
        // automatically fall back to _regular at runtime if they are missing.
        genStep++;
        requestUpdate();
        return;
      }

      std::string familyName = selectedFamilyPath.substr(selectedFamilyPath.rfind('/') + 1);

      // Ensure parent /.fonts/ exists before creating the family subdir
      Storage.mkdir("/.fonts");
      char outDir[256];
      snprintf(outDir, sizeof(outDir), "/.fonts/%s", familyName.c_str());
      Storage.mkdir(outDir);

      char outPath[256];
      snprintf(outPath, sizeof(outPath), "/.fonts/%s/%d_%s.epdfont", familyName.c_str(), customPt[slot], styleSuffix);

      bool ok = RuntimeFontConverter::generateAndSaveToFile(ttfPath.c_str(), customPt[slot], SETTINGS.textAntiAliasing,
                                                            outPath);
      if (!ok && genOk) {
        genOk = false;
        genError = std::string("Failed: ") + styleSuffix + " pt" + std::to_string(customPt[slot]);
      }

      genStep++;
      requestUpdate();
    } else {
      // Generation complete — release power lock
      pwrLock.reset();
      if (genOk) {
        std::string familyName = selectedFamilyPath.substr(selectedFamilyPath.rfind('/') + 1);
        String dotFontsBase = String("/.fonts/") + familyName.c_str();
        EpdFontFileLoader::setFamily(dotFontsBase, customPt);
        SETTINGS.fontFamily = CrossPointSettings::CUSTOM_FONT;
        SETTINGS.saveToFile();
        EpdFontFileLoader::clearCache();
      }
      state = genOk ? State::DONE_OK : State::DONE_ERROR;
      sizeConfigRow = 0;
      requestUpdate();
    }
    return;
  }

  // ── DONE_OK / DONE_ERROR ──────────────────────────────────────────────────
  if (state == State::DONE_OK || state == State::DONE_ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    buttonNavigator.onPreviousRelease([this] {
      if (sizeConfigRow > 0) {
        sizeConfigRow--;
        requestUpdate();
      }
    });
    buttonNavigator.onNextRelease([this] {
      if (sizeConfigRow < 2) {
        sizeConfigRow++;
        requestUpdate();
      }
    });
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (sizeConfigRow == 0) {
        finish();
      } else if (sizeConfigRow == 1) {
        std::string familyName = selectedFamilyPath.substr(selectedFamilyPath.rfind('/') + 1);
        std::string dotFontsDir = std::string("/.fonts/") + familyName;
        deleteDirectory(dotFontsDir.c_str());
        EpdFontFileLoader::clearFamily();
        if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FONT) {
          SETTINGS.fontFamily = CrossPointSettings::BOOKERLY;
          SETTINGS.saveToFile();
        }
        finish();
      } else {
        state = State::BROWSER;
        sizeConfigRow = 0;
        requestUpdate();
      }
    }
    return;
  }
}

// ─── render ──────────────────────────────────────────────────────────────────

void CustomFontActivity::render(RenderLock&& lock) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  switch (state) {
    case State::BROWSER:
    case State::CONFIRM:
      renderBrowser(pageWidth, pageHeight, metrics);
      break;
    case State::SIZE_CONFIG:
      renderSizeConfig(pageWidth, pageHeight, metrics);
      break;
    case State::GENERATING:
      renderGenerating(pageWidth, pageHeight, metrics);
      break;
    case State::DONE_OK:
      renderDone(pageWidth, pageHeight, metrics, true);
      break;
    case State::DONE_ERROR:
      renderDone(pageWidth, pageHeight, metrics, false);
      break;
  }
}

// ─── renderBrowser ───────────────────────────────────────────────────────────

void CustomFontActivity::renderBrowser(int w, int h, const ThemeMetrics& metrics) {
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight}, "Select Font Family");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentH = h - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (folders.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, "No font folders found in /fonts",
                      true);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, w, contentH}, folders.size(), selectorIndex,
        [this](int index) {
          const auto& info = folders[index];
          char label[128];
          const char* typeTag = "";
          if (info.hasTtf && info.hasEpd)
            typeTag = " [BOTH]";
          else if (info.hasEpd)
            typeTag = " [EPD]";
          else if (info.hasTtf)
            typeTag = " [TTF]";

          snprintf(label, sizeof(label), "%s%s", info.name.c_str(), typeTag);
          return std::string(label);
        },
        nullptr, [](int) { return UIIcon::Folder; });
  }

  // CONFIRM overlay
  if (state == State::CONFIRM) {
    std::string familyName = selectedFamilyPath.substr(selectedFamilyPath.rfind('/') + 1);
    char msg[128];
    if (isImportMode) {
      snprintf(msg, sizeof(msg), "Pre-converted font found");
    } else {
      snprintf(msg, sizeof(msg), "Cache exists for %s", familyName.c_str());
    }

    const int boxW = w - 60;
    const int boxH = isImportMode ? 120 : 180;
    const int boxX = 30;
    const int boxY = h / 2 - boxH / 2;
    renderer.fillRect(boxX, boxY, boxW, boxH, true);
    renderer.drawRect(boxX, boxY, boxW, boxH);
    renderer.drawCenteredText(UI_12_FONT_ID, boxY + 16, msg, false, EpdFontFamily::BOLD);

    if (isImportMode) {
      const char* opts[] = {"Import", "Cancel"};
      for (int i = 0; i < 2; ++i) {
        int y = boxY + 50 + i * 30;
        bool sel = (sizeConfigRow == i);
        if (sel) renderer.drawText(UI_12_FONT_ID, boxX + 14, y, "\xe2\x96\xb6", false, EpdFontFamily::BOLD);
        renderer.drawText(sel ? UI_12_FONT_ID : UI_10_FONT_ID, boxX + 36, y, opts[i], false,
                          sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      }
    } else {
      const auto& info = folders[selectorIndex];
      const char* btn2Name = info.hasEpd ? "Re-import" : "Regenerate";
      const char* opts[] = {"Select", btn2Name, "Delete Cache", "Back"};
      for (int i = 0; i < 4; ++i) {
        int y = boxY + 50 + i * 30;
        bool sel = (sizeConfigRow == i);
        if (sel) renderer.drawText(UI_12_FONT_ID, boxX + 14, y, "\xe2\x96\xb6", false, EpdFontFamily::BOLD);
        renderer.drawText(sel ? UI_12_FONT_ID : UI_10_FONT_ID, boxX + 36, y, opts[i], false,
                          sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      }
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

// ─── renderSizeConfig ────────────────────────────────────────────────────────

void CustomFontActivity::renderSizeConfig(int w, int h, const ThemeMetrics& metrics) {
  std::string familyName = selectedFamilyPath.substr(selectedFamilyPath.rfind('/') + 1);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight}, ("Set sizes: " + familyName).c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // Style feedback — two short lines to stay within screen width
  char foundLine[48];
  snprintf(foundLine, sizeof(foundLine), "Found %d/%d styles", foundStyleCount, NUM_STYLES);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop, foundLine);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + renderer.getLineHeight(UI_10_FONT_ID),
                    "Up/Down: select   Left/Right: adjust size");

  const int rowsTop = contentTop + 2 * renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing + 10;
  const int rowH = renderer.getLineHeight(UI_12_FONT_ID) + 16;

  const char* hints[] = {"(S)", "(M)", "(L)", "(XL)"};
  for (int i = 0; i < NUM_SIZE_SLOTS; ++i) {
    int y = rowsTop + i * rowH;
    bool sel = (sizeConfigRow == i);

    char label[64];
    snprintf(label, sizeof(label), "%s  %s", hints[i], SIZE_NAMES[i]);
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, label, true,
                      sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

    // Point size value (right-aligned area)
    char ptStr[16];
    snprintf(ptStr, sizeof(ptStr), sel ? "[ %d pt ]" : "%d pt", customPt[i]);
    renderer.drawText(UI_12_FONT_ID, w - 110, y, ptStr, true, sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }

  // Generate button
  const int genY = rowsTop + NUM_SIZE_SLOTS * rowH + metrics.verticalSpacing;
  bool genSel = (sizeConfigRow >= NUM_SIZE_SLOTS);
  renderer.drawCenteredText(UI_12_FONT_ID, genY, genSel ? ">>  Generate  <<" : "[ Generate ]", true,
                            genSel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

  // Font Preview Area
  int previewY = genY + rowH + metrics.verticalSpacing + 25;
  renderer.drawCenteredText(UI_10_FONT_ID, previewY, "Preview (confirm to generate)", true, EpdFontFamily::BOLD);
  previewY += renderer.getLineHeight(UI_10_FONT_ID) + 12;

  if (previewFamily && renderer.hasFont(PREVIEW_FONT_ID)) {
    const char* previewString = "The quick brown fox jumps over the lazy dog 0123456789";
    auto lines = renderer.wrappedText(PREVIEW_FONT_ID, previewString, w - metrics.contentSidePadding * 4, 3,
                                      EpdFontFamily::REGULAR);
    for (const auto& line : lines) {
      renderer.drawText(PREVIEW_FONT_ID, metrics.contentSidePadding * 2, previewY, line.c_str(), true);
      previewY += renderer.getLineHeight(PREVIEW_FONT_ID) + 6;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "Size -", "Size +");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

// ─── renderGenerating ────────────────────────────────────────────────────────

void CustomFontActivity::renderGenerating(int w, int h, const ThemeMetrics& metrics) {
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight}, "Generating Fonts");

  const int cy = h / 2;
  int step = std::min(genStep, totalSteps);
  int slot = (step < totalSteps) ? step / NUM_STYLES : NUM_SIZE_SLOTS - 1;
  int styleIdx = (step < totalSteps) ? step % NUM_STYLES : NUM_STYLES - 1;

  char status[128];
  snprintf(status, sizeof(status), "%s %dpt  (%s)", SIZE_NAMES[slot], customPt[slot], STYLE_SUFFIXES[styleIdx]);
  renderer.drawCenteredText(UI_12_FONT_ID, cy - 50, status, true, EpdFontFamily::REGULAR);

  const int barW = w - 120, barH = 16, barX = 60, barY = cy - 10;
  GUI.drawProgressBar(renderer, Rect{barX, barY, barW, barH}, step, totalSteps);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// ─── renderDone ──────────────────────────────────────────────────────────────

void CustomFontActivity::renderDone(int w, int h, const ThemeMetrics& metrics, bool ok) {
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight},
                 ok ? "Font Ready!" : "Generation Failed");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;

  if (!genError.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop, genError.c_str(), true);
  }
  if (ok) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 28,
                      "Open a book to preview the new font.", true);
  }

  const char* opts[] = {"OK", "Delete Cache", "Back"};
  const int optRowH = renderer.getLineHeight(UI_12_FONT_ID) + 8;
  const int optStartY = contentTop + 70;
  for (int i = 0; i < 3; ++i) {
    int y = optStartY + i * optRowH;
    bool sel = (sizeConfigRow == i);
    if (sel)
      renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding - 2, y, "\xe2\x96\xb6", true, EpdFontFamily::BOLD);
    renderer.drawText(sel ? UI_12_FONT_ID : UI_10_FONT_ID, metrics.contentSidePadding + 18, y, opts[i], true,
                      sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#endif  // ENABLE_CUSTOM_FONTS
