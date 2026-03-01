#include "EpdFontFileLoader.h"

#ifdef ENABLE_CUSTOM_FONTS

#include <HalStorage.h>
#include <Logging.h>
#include <Preferences.h>

#include "EpdFontSerializer.h"
#include "EpdStreamFont.h"

// ─── Static member definitions ────────────────────────────────────────────────

EpdFontFamily* EpdFontFileLoader::cachedFamily = nullptr;
EpdFontFileLoader::SizeSlot EpdFontFileLoader::cachedSlot = EpdFontFileLoader::SMALL;
bool EpdFontFileLoader::cacheValid = false;
String EpdFontFileLoader::familyBasePath = "";
int EpdFontFileLoader::customPt[EpdFontFileLoader::SIZE_SLOT_COUNT] = {12, 14, 16, 18};
bool EpdFontFileLoader::initialized = false;

static const char* NVS_NAMESPACE = "sd_font";
static const char* NVS_PATH_KEY = "path";
static const char* NVS_PT_S_KEY = "pt_s";
static const char* NVS_PT_M_KEY = "pt_m";
static const char* NVS_PT_L_KEY = "pt_l";
static const char* NVS_PT_XL_KEY = "pt_xl";

static const int SD_FONT_IDS[EpdFontFileLoader::SIZE_SLOT_COUNT] = {
    EpdFontFileLoader::FONT_ID_SMALL,
    EpdFontFileLoader::FONT_ID_MEDIUM,
    EpdFontFileLoader::FONT_ID_LARGE,
    EpdFontFileLoader::FONT_ID_EXTRA_LARGE,
};

// ─── init ─────────────────────────────────────────────────────────────────────

void EpdFontFileLoader::init() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  familyBasePath = prefs.getString(NVS_PATH_KEY, "");
  customPt[SMALL] = prefs.getInt(NVS_PT_S_KEY, 12);
  customPt[MEDIUM] = prefs.getInt(NVS_PT_M_KEY, 14);
  customPt[LARGE] = prefs.getInt(NVS_PT_L_KEY, 16);
  customPt[EXTRA_LARGE] = prefs.getInt(NVS_PT_XL_KEY, 18);
  prefs.end();

  initialized = true;

  if (familyBasePath.length() > 0) {
    char checkPath[256];
    snprintf(checkPath, sizeof(checkPath), "%s/%d_regular.epdfont", familyBasePath.c_str(), customPt[MEDIUM]);
    if (!Storage.exists(checkPath)) {
      LOG_ERR("FFL", "Configured font missing: %s. Clearing cache.", checkPath);
      clearFamily();
    } else {
      LOG_INF("FFL", "Custom SD font configured: %s [%d/%d/%d/%d]", familyBasePath.c_str(), customPt[SMALL],
              customPt[MEDIUM], customPt[LARGE], customPt[EXTRA_LARGE]);
    }
  } else {
    LOG_DBG("FFL", "No custom SD font configured");
  }
}

// ─── isAvailable ──────────────────────────────────────────────────────────────

bool EpdFontFileLoader::isAvailable() { return initialized && familyBasePath.length() > 0; }

// ─── getFamilyName ────────────────────────────────────────────────────────────

String EpdFontFileLoader::getFamilyName() {
  if (familyBasePath.length() == 0) return "";
  int last = familyBasePath.lastIndexOf('/');
  return (last >= 0) ? familyBasePath.substring(last + 1) : familyBasePath;
}

// ─── getPointSize ─────────────────────────────────────────────────────────────

int EpdFontFileLoader::getPointSize(SizeSlot slot) {
  if (slot < 0 || slot >= SIZE_SLOT_COUNT) return 14;
  return customPt[slot];
}

// ─── getFontId ────────────────────────────────────────────────────────────────

int EpdFontFileLoader::getFontId(SizeSlot slot) {
  if (slot < 0 || slot >= SIZE_SLOT_COUNT) return FONT_ID_MEDIUM;
  return SD_FONT_IDS[slot];
}

// ─── loadStyle ────────────────────────────────────────────────────────────────

EpdFont* EpdFontFileLoader::loadStyle(SizeSlot slot, const char* styleSuffix) {
  char path[256];
  snprintf(path, sizeof(path), "%s/%d_%s.epdfont", familyBasePath.c_str(), customPt[slot], styleSuffix);

  if (!Storage.exists(path)) {
    LOG_DBG("FFL", "Style file not found: %s", path);
    return nullptr;
  }

  return EpdStreamFont::load(path);
}

// ─── getFamily ────────────────────────────────────────────────────────────────

EpdFontFamily* EpdFontFileLoader::getFamily(SizeSlot slot) {
  if (!isAvailable()) return nullptr;

  // Return cached if same slot and still valid
  if (cacheValid && cachedSlot == slot && cachedFamily) return cachedFamily;

  // Free old cache
  freeCachedFamily();

  LOG_INF("FFL", "Loading SD font family for slot %d (pt=%d)", slot, customPt[slot]);

  EpdFont* regular = loadStyle(slot, "regular");
  if (!regular) {
    LOG_ERR("FFL", "Failed to load regular style for slot %d", slot);
    return nullptr;
  }

  EpdFont* bold = loadStyle(slot, "bold");
  EpdFont* italic = loadStyle(slot, "italic");
  EpdFont* boldItalic = loadStyle(slot, "bolditalic");

  // Fallbacks: if style missing use regular
  if (!bold) bold = regular;
  if (!italic) italic = regular;
  if (!boldItalic) boldItalic = (bold != regular) ? bold : regular;

  // Allocate EpdFontFamily in PSRAM
  EpdFontFamily* fam = (EpdFontFamily*)heap_caps_malloc(sizeof(EpdFontFamily), MALLOC_CAP_SPIRAM);
  if (!fam) {
    LOG_ERR("FFL", "PSRAM alloc for EpdFontFamily failed");
    delete regular;
    if (bold != regular) delete bold;
    if (italic != regular && italic != bold) delete italic;
    if (boldItalic != regular && boldItalic != bold && boldItalic != italic) delete boldItalic;
    return nullptr;
  }

  new (fam) EpdFontFamily(regular, bold, italic, boldItalic);
  cachedFamily = fam;
  cachedSlot = slot;
  cacheValid = true;

  LOG_INF("FFL", "SD font family loaded (slot %d)", slot);
  return cachedFamily;
}

// ─── clearCache ───────────────────────────────────────────────────────────────

void EpdFontFileLoader::clearCache() { freeCachedFamily(); }

void EpdFontFileLoader::freeCachedFamily() {
  if (!cacheValid || !cachedFamily) return;

  auto freeUnique = [](const EpdFont* f, const EpdFont* ref1, const EpdFont* ref2, const EpdFont* ref3) {
    if (f && f != ref1 && f != ref2 && f != ref3) delete f;
  };

  const EpdFont* r = cachedFamily->getFontPtr(EpdFontFamily::REGULAR);
  const EpdFont* b = cachedFamily->getFontPtr(EpdFontFamily::BOLD);
  const EpdFont* i = cachedFamily->getFontPtr(EpdFontFamily::ITALIC);
  const EpdFont* bi = cachedFamily->getFontPtr(EpdFontFamily::BOLD_ITALIC);

  freeUnique(r, nullptr, nullptr, nullptr);
  freeUnique(b, r, nullptr, nullptr);
  freeUnique(i, r, b, nullptr);
  freeUnique(bi, r, b, i);

  heap_caps_free(cachedFamily);
  cachedFamily = nullptr;
  cacheValid = false;
}

// ─── setFamily ────────────────────────────────────────────────────────────────

bool EpdFontFileLoader::setFamily(const String& basePath, const int pts[SIZE_SLOT_COUNT]) {
  clearCache();
  familyBasePath = basePath;
  for (int i = 0; i < SIZE_SLOT_COUNT; i++) customPt[i] = pts[i];

  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(NVS_PATH_KEY, familyBasePath);
  prefs.putInt(NVS_PT_S_KEY, customPt[SMALL]);
  prefs.putInt(NVS_PT_M_KEY, customPt[MEDIUM]);
  prefs.putInt(NVS_PT_L_KEY, customPt[LARGE]);
  prefs.putInt(NVS_PT_XL_KEY, customPt[EXTRA_LARGE]);
  prefs.end();

  LOG_INF("FFL", "Custom SD font family saved: %s [%d/%d/%d/%d]", familyBasePath.c_str(), customPt[SMALL],
          customPt[MEDIUM], customPt[LARGE], customPt[EXTRA_LARGE]);
  return true;
}

// ─── clearFamily ──────────────────────────────────────────────────────────────

void EpdFontFileLoader::clearFamily() {
  clearCache();
  familyBasePath = "";
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
}

#endif  // ENABLE_CUSTOM_FONTS
