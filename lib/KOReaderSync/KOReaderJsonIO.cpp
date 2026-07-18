#include "KOReaderJsonIO.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include "KOReaderCredentialStore.h"

namespace KOReaderJsonIO {

namespace {
// Config schema version stamped into the JSON. Bumped when a change to defaults
// would alter behavior for existing configs (#2587: default server switched to
// crosspoint-sync). Kept in sync with the value written by save() below.
constexpr uint8_t CONFIG_VERSION = 2;
// The default sync server before v2. A pre-v2 config with credentials and no
// explicit serverUrl was implicitly syncing here, so it is pinned on upgrade.
constexpr char LEGACY_DEFAULT_SERVER_URL[] = "https://sync.koreader.rocks:443";
}  // namespace

bool save(const KOReaderCredentialStore& store, const char* path) {
  JsonDocument doc;
  doc["cfgVersion"] = CONFIG_VERSION;
  doc["username"] = store.getUsername();
  doc["password_obf"] = obfuscation::obfuscateToBase64(store.getPassword());
  doc["serverUrl"] = store.getServerUrl();
  doc["matchMethod"] = static_cast<uint8_t>(store.getMatchMethod());
  doc["sendMetadata"] = store.getSendMetadata();
  doc["syncBehavior"] = static_cast<uint8_t>(store.getSyncBehavior());

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool load(KOReaderCredentialStore& store, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("KRS", "JSON parse error: %s", error.c_str());
    return false;
  }

  std::string user = doc["username"] | std::string("");

  bool ok = false;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &ok);
  if (!ok || pass.empty()) {
    pass = doc["password"] | std::string("");
    if (!pass.empty() && needsResave) *needsResave = true;
  }

  store.setCredentials(user, pass);
  store.setServerUrl(doc["serverUrl"] | std::string(""));

  // The default server changed in config v2 (sync.koreader.rocks -> crosspoint-sync).
  // A pre-v2 config with credentials and no explicit URL was actively syncing against
  // the old default — pin that URL so the upgrade doesn't switch servers out from
  // under the user. Fresh setups keep the new default. Stamp cfgVersion (via resave)
  // so this runs once.
  const uint8_t cfgVersion = doc["cfgVersion"] | (uint8_t)1;
  if (cfgVersion < CONFIG_VERSION) {
    if (store.getServerUrl().empty() && store.hasCredentials()) {
      LOG_DBG("KRS", "Pre-v2 config used the old default server; pinning %s", LEGACY_DEFAULT_SERVER_URL);
      store.setServerUrl(LEGACY_DEFAULT_SERVER_URL);
    }
    if (needsResave) *needsResave = true;
  }

  uint8_t method = doc["matchMethod"] | (uint8_t)0;
  store.setMatchMethod(static_cast<DocumentMatchMethod>(method));

  store.setSendMetadata(doc["sendMetadata"] | false);

  // syncBehavior defaults to ASK_EVERY_TIME when absent (older files) or invalid,
  // and flags a resave so the field is written back.
  const JsonVariantConst behaviorValue = doc["syncBehavior"];
  const uint8_t behavior = behaviorValue | static_cast<uint8_t>(KOReaderSyncBehavior::ASK_EVERY_TIME);
  if (behavior <= static_cast<uint8_t>(KOReaderSyncBehavior::SMART)) {
    store.setSyncBehavior(static_cast<KOReaderSyncBehavior>(behavior));
    if (behaviorValue.isNull() && needsResave) *needsResave = true;
  } else {
    LOG_DBG("KRS", "Invalid syncBehavior %u in JSON, resetting to ASK_EVERY_TIME", behavior);
    store.setSyncBehavior(KOReaderSyncBehavior::ASK_EVERY_TIME);
    if (needsResave) *needsResave = true;
  }

  return true;
}

}  // namespace KOReaderJsonIO
