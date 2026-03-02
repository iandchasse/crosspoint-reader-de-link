#pragma once

#ifdef ENABLE_CUSTOM_FONTS

#include <memory>
#include <string>
#include <vector>

#include "EpdFontFileLoader.h"
#include "HalPowerManager.h"
#include "activities/Activity.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

class CustomFontActivity final : public Activity {
 public:
  enum class State { BROWSER, CONFIRM, SIZE_CONFIG, GENERATING, DONE_OK, DONE_ERROR };
  struct FolderInfo {
    std::string name;
    bool hasTtf;
    bool hasEpd;
  };

  explicit CustomFontActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("CustomFont", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override;
  bool skipLoopDelay() override;
  void render(RenderLock&&) override;

 private:
  // ── Input debounce ────────────────────────────────────────────────────────
  unsigned long enterTimeMs = 0;
  static constexpr unsigned long INPUT_DEBOUNCE_MS = 500;

  // ── Browser state ─────────────────────────────────────────────────────────
  ButtonNavigator buttonNavigator;
  std::vector<FolderInfo> folders;  // only font-family FOLDERS shown
  std::string basepath = "/fonts";
  std::string selectedFamilyPath;
  bool isImportMode = false;
  // Resolved TTF paths for each style (empty = not found / use regular)
  std::string resolvedTtfPaths[4];
  int foundStyleCount = 0;
  size_t selectorIndex = 0;

  // ── Size configuration state ──────────────────────────────────────────────
  static constexpr int NUM_SIZE_SLOTS = EpdFontFileLoader::SIZE_SLOT_COUNT;
  int customPt[NUM_SIZE_SLOTS] = {12, 14, 16, 18};
  int sizeConfigRow = 0;  // 0-3 = size rows, 4 = Generate button

  // ── Preview state ─────────────────────────────────────────────────────────
  EpdFont* previewFont = nullptr;
  EpdFontFamily* previewFamily = nullptr;
  static constexpr int PREVIEW_FONT_ID = -2000000005;  // temporary ID for previews
  int lastPreviewedPt[NUM_SIZE_SLOTS] = {0};
  int lastPreviewSlot = -1;

  // ── Generation state ──────────────────────────────────────────────────────
  int genStep = 0;
  int totalSteps = 0;
  bool genOk = false;
  std::string genError;
  std::unique_ptr<HalPowerManager::Lock> pwrLock;  // held during generation

  // ── State machine ─────────────────────────────────────────────────────────
  State state = State::BROWSER;

  // ── Private helpers ───────────────────────────────────────────────────────
  void loadFolders();                                   // list only directories in basepath
  FolderInfo checkFolderType(const std::string& name);  // check contents of a folder
  bool scanFamilyStyles(const std::string& familyDir);  // populate resolvedTtfPaths
  size_t findEntry(const std::string& name) const;
  void onSelectFolder(const std::string& familyDir);
  void startGeneration();

  void updatePreview(int slot);
  void cleanupPreview();

  void renderBrowser(int w, int h, const ThemeMetrics& metrics);
  void renderSizeConfig(int w, int h, const ThemeMetrics& metrics);
  void renderGenerating(int w, int h, const ThemeMetrics& metrics);
  void renderDone(int w, int h, const ThemeMetrics& metrics, bool ok);
};

#endif
