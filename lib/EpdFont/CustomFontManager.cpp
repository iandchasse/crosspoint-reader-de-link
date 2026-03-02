#include "CustomFontManager.h"

#ifdef ENABLE_CUSTOM_FONTS

#include <Arduino.h>
#include <FS.h>
#include <Logging.h>
#include <Preferences.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "RuntimeFontConverter.h"

EpdFontFamily* CustomFontManager::customFontFamily = nullptr;
static const char* NVS_NAMESPACE = "custom_font";

void CustomFontManager::init() { LOG_DBG("CFM", "CustomFontManager (deprecated) initialized"); }

EpdFontFamily* CustomFontManager::getActiveCustomFont() { return customFontFamily; }

bool CustomFontManager::setActiveCustomFont(const String& ttfPath, int sizePt, bool is2Bit) {
  LOG_INF("CFM", "Setting active custom font: %s (size: %d)", ttfPath.c_str(), sizePt);

  // Determine the base path by stripping extension and any style suffixes
  String base = ttfPath;
  if (base.endsWith(".ttf"))
    base = base.substring(0, base.length() - 4);
  else if (base.endsWith(".TTF"))
    base = base.substring(0, base.length() - 4);

  // Common suffixes to strip to find the true family base
  static const char* suffixes[] = {"_reg",    "_bold",       "_italic", "_bolditalic", "-Regular", "-Bold",
                                   "-Italic", "-BoldItalic", "Regular", "Bold",        "Italic",   "BoldItalic"};

  for (const char* suffix : suffixes) {
    if (base.endsWith(suffix)) {
      base = base.substring(0, base.length() - strlen(suffix));
      break;
    }
    // Case insensitive check for convenience
    String baseLower = base;
    baseLower.toLowerCase();
    String suffixLower = String(suffix);
    suffixLower.toLowerCase();
    if (baseLower.endsWith(suffixLower)) {
      base = base.substring(0, base.length() - strlen(suffix));
      break;
    }
  }

  LOG_DBG("CFM", "Detected family base path: %s", base.c_str());

  EpdFont* regular = RuntimeFontConverter::generateEpdFontFromPath(ttfPath.c_str(), sizePt, is2Bit);
  // If we selected a bold/italic file directly, it's still our "regular" (primary) for this load attempt
  // but we prefer to find the actual _reg if we can to be consistent?
  // Actually, we'll use the user's selection as the anchor.

  if (!regular) {
    // If ttfPath failed, try the base + _reg just in case
    regular = RuntimeFontConverter::generateEpdFontFromPath((base + "_reg.ttf").c_str(), sizePt, is2Bit);
    if (!regular) regular = RuntimeFontConverter::generateEpdFontFromPath((base + "_reg.TTF").c_str(), sizePt, is2Bit);
  }

  if (!regular) {
    LOG_ERR("CFM", "Failed to load primary font: %s", ttfPath.c_str());
    return false;
  }

  // Look for variants based on the family base
  EpdFont* bold = RuntimeFontConverter::generateEpdFontFromPath((base + "_bold.ttf").c_str(), sizePt, is2Bit);
  if (!bold) bold = RuntimeFontConverter::generateEpdFontFromPath((base + "_bold.TTF").c_str(), sizePt, is2Bit);
  if (!bold) bold = RuntimeFontConverter::generateEpdFontFromPath((base + "-Bold.ttf").c_str(), sizePt, is2Bit);

  EpdFont* italic = RuntimeFontConverter::generateEpdFontFromPath((base + "_italic.ttf").c_str(), sizePt, is2Bit);
  if (!italic) italic = RuntimeFontConverter::generateEpdFontFromPath((base + "_italic.TTF").c_str(), sizePt, is2Bit);
  if (!italic) italic = RuntimeFontConverter::generateEpdFontFromPath((base + "-Italic.ttf").c_str(), sizePt, is2Bit);

  EpdFont* boldItalic =
      RuntimeFontConverter::generateEpdFontFromPath((base + "_bolditalic.ttf").c_str(), sizePt, is2Bit);
  if (!boldItalic)
    boldItalic = RuntimeFontConverter::generateEpdFontFromPath((base + "_bolditalic.TTF").c_str(), sizePt, is2Bit);
  if (!boldItalic)
    boldItalic = RuntimeFontConverter::generateEpdFontFromPath((base + "-BoldItalic.ttf").c_str(), sizePt, is2Bit);

  // Fallbacks
  if (!bold) bold = regular;
  if (!italic) italic = regular;
  if (!boldItalic) boldItalic = (bold != regular) ? bold : regular;

  // Allocate family structure
  EpdFontFamily* newFamily = (EpdFontFamily*)heap_caps_malloc(sizeof(EpdFontFamily), MALLOC_CAP_SPIRAM);
  if (!newFamily) {
    RuntimeFontConverter::freeEpdFont(regular);
    if (bold != regular) RuntimeFontConverter::freeEpdFont(bold);
    if (italic != regular && italic != bold) RuntimeFontConverter::freeEpdFont(italic);
    if (boldItalic != regular && boldItalic != bold && boldItalic != italic)
      RuntimeFontConverter::freeEpdFont(boldItalic);
    return false;
  }

  new (newFamily) EpdFontFamily(regular, bold, italic, boldItalic);

  // Clear old
  clearCustomFont();

  customFontFamily = newFamily;

  LOG_INF("CFM", "Custom font family successfully loaded and active");
  return true;
}

void CustomFontManager::clearCustomFont() {
  if (customFontFamily) {
    auto fReg = customFontFamily->getFontPtr(EpdFontFamily::REGULAR);
    auto fBold = customFontFamily->getFontPtr(EpdFontFamily::BOLD);
    auto fItalic = customFontFamily->getFontPtr(EpdFontFamily::ITALIC);
    auto fBoldItalic = customFontFamily->getFontPtr(EpdFontFamily::BOLD_ITALIC);

    if (fReg) RuntimeFontConverter::freeEpdFont(const_cast<EpdFont*>(fReg));
    if (fBold && fBold != fReg) RuntimeFontConverter::freeEpdFont(const_cast<EpdFont*>(fBold));
    if (fItalic && fItalic != fReg && fItalic != fBold)
      RuntimeFontConverter::freeEpdFont(const_cast<EpdFont*>(fItalic));
    if (fBoldItalic && fBoldItalic != fReg && fBoldItalic != fBold && fBoldItalic != fItalic)
      RuntimeFontConverter::freeEpdFont(const_cast<EpdFont*>(fBoldItalic));

    heap_caps_free(customFontFamily);
    customFontFamily = nullptr;
  }
}

size_t CustomFontManager::getTotalRamUsage() {
  if (!customFontFamily) return 0;
  size_t total = 0;
  const EpdFont* f;
  f = customFontFamily->getFontPtr(EpdFontFamily::REGULAR);
  if (f) total += f->data->totalAllocatedSize;

  auto fReg = f;
  f = customFontFamily->getFontPtr(EpdFontFamily::BOLD);
  if (f && f != fReg) total += f->data->totalAllocatedSize;

  auto fBold = f;
  f = customFontFamily->getFontPtr(EpdFontFamily::ITALIC);
  if (f && f != fReg && f != fBold) total += f->data->totalAllocatedSize;

  auto fItalic = f;
  f = customFontFamily->getFontPtr(EpdFontFamily::BOLD_ITALIC);
  if (f && f != fReg && f != fBold && f != fItalic) total += f->data->totalAllocatedSize;

  return total;
}

#endif  // ENABLE_CUSTOM_FONTS
