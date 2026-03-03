#include "FontDownloadActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_wifi.h>
#include <fcntl.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char preconvertedUrl[] =
    "https://api.github.com/repos/iandchasse/crosspoint-reader-minRead/contents/lib/EpdFont/scripts/"
    "fonts_preconverted?ref=C3-custom-font-ttf";
constexpr char ttfUrl[] =
    "https://api.github.com/repos/iandchasse/crosspoint-reader-minRead/contents/lib/EpdFont/builtinFonts"
    "/source?ref=C3-custom-font-ttf";

extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

esp_err_t string_event_handler(esp_http_client_event_t* event) {
  if (event->event_id == HTTP_EVENT_ON_DATA) {
    std::string* buf = static_cast<std::string*>(event->user_data);
    buf->append((char*)event->data, event->data_len);
  }
  return ESP_OK;
}

bool fetchUrlSecure(const std::string& url, std::string& response) {
  esp_http_client_config_t client_config = {
      .url = url.c_str(),
      .event_handler = string_event_handler,
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };
  client_config.user_data = &response;

  esp_http_client_handle_t client = esp_http_client_init(&client_config);
  if (!client) return false;
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  esp_err_t err = esp_http_client_perform(client);
  esp_http_client_cleanup(client);
  return (err == ESP_OK);
}

struct DownloadContext {
  std::string destPath;
  FsFile file;
  size_t downloaded = 0;
  size_t total = 0;
  std::function<void(size_t, size_t)> progressCb;
  bool success = true;
};

esp_err_t file_event_handler(esp_http_client_event_t* event) {
  DownloadContext* ctx = static_cast<DownloadContext*>(event->user_data);
  if (event->event_id == HTTP_EVENT_ON_DATA) {
    if (!ctx->file) {
      ctx->file = Storage.open(ctx->destPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
      if (!ctx->file) {
        ctx->success = false;
        return ESP_FAIL;
      }
    }
    size_t written = ctx->file.write((uint8_t*)event->data, event->data_len);
    if (written != (size_t)event->data_len) {
      ctx->success = false;
      return ESP_FAIL;
    }
    ctx->downloaded += event->data_len;
    if (ctx->progressCb) {
      ctx->progressCb(ctx->downloaded, ctx->total);
    }
  }
  return ESP_OK;
}

bool downloadFileSecure(const std::string& url, const std::string& destPath, size_t totalSize,
                        std::function<void(size_t, size_t)> progressCb) {
  DownloadContext ctx;
  ctx.destPath = destPath;
  ctx.total = totalSize;
  ctx.progressCb = progressCb;

  esp_http_client_config_t client_config = {
      .url = url.c_str(),
      .event_handler = file_event_handler,
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };
  client_config.timeout_ms = 15000;
  client_config.user_data = &ctx;

  esp_http_client_handle_t client = esp_http_client_init(&client_config);
  if (!client) return false;
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (ctx.file) ctx.file.close();
  return (err == ESP_OK && ctx.success && status == 200);
}
}  // namespace

void FontDownloadActivity::onEnter() {
  Activity::onEnter();

  LOG_DBG("FONTDL", "Turning on WiFi...");
  WiFi.mode(WIFI_STA);

  LOG_DBG("FONTDL", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  Activity::onExit();

  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("FONTDL", "WiFi connection failed, exiting");
    finish();
    return;
  }

  LOG_DBG("FONTDL", "WiFi connected, fetching font directories");

  {
    RenderLock lock(*this);
    state = CHOOSE_TYPE;
    preconvertedType = true;
  }
  requestUpdate();
}

bool FontDownloadActivity::fetchFontFamilies() {
  const char* url = preconvertedType ? preconvertedUrl : ttfUrl;
  std::string response;
  if (!fetchUrlSecure(url, response)) {
    return false;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, response);
  if (error) {
    LOG_ERR("FONTDL", "JSON parse failed: %s", error.c_str());
    return false;
  }

  if (!doc.is<JsonArray>()) {
    LOG_ERR("FONTDL", "Expected JSON array at root");
    return false;
  }

  for (JsonObject item : doc.as<JsonArray>()) {
    if (item["type"] == "dir") {
      FontFamily family;
      family.name = item["name"].as<std::string>();
      family.url = item["url"].as<std::string>();
      fontFamilies.push_back(std::move(family));
    }
  }

  if (fontFamilies.empty()) {
    LOG_ERR("FONTDL", "No font directories found");
    return false;
  }
  return true;
}

bool FontDownloadActivity::fetchFontFiles(FontFamily& family) {
  std::string response;
  if (!fetchUrlSecure(family.url, response)) {
    return false;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, response);
  if (error) {
    LOG_ERR("FONTDL", "JSON parse failed for family %s: %s", family.name.c_str(), error.c_str());
    return false;
  }

  if (!doc.is<JsonArray>()) {
    LOG_ERR("FONTDL", "Expected JSON array for contents of %s", family.name.c_str());
    return false;
  }

  for (JsonObject item : doc.as<JsonArray>()) {
    if (item["type"] == "file") {
      std::string name = item["name"].as<std::string>();
      if (name.find(".epdfont") != std::string::npos || name.find(".ttf") != std::string::npos ||
          name.find(".otf") != std::string::npos) {
        FontFile file;
        file.name = name;
        file.downloadUrl = item["download_url"].as<std::string>();
        file.size = item["size"].as<size_t>();
        family.files.push_back(std::move(file));
      }
    }
  }

  return !family.files.empty();
}

bool FontDownloadActivity::downloadFontFiles() {
  size_t downloadedFiles = 0;
  totalBytesOverall = 0;
  totalBytesDownloaded = 0;

  for (size_t i = 0; i < fontFamilies.size(); ++i) {
    if (selectedFamilies[i]) {
      if (!fetchFontFiles(fontFamilies[i])) return false;
      for (const auto& file : fontFamilies[i].files) {
        totalBytesOverall += file.size;
      }
    }
  }

  if (totalBytesOverall == 0) return true;

  for (currentFamilyIndex = 0; currentFamilyIndex < fontFamilies.size(); ++currentFamilyIndex) {
    if (!selectedFamilies[currentFamilyIndex]) continue;

    const auto& family = fontFamilies[currentFamilyIndex];

    if (!Storage.exists("/fonts")) {
      Storage.mkdir("/fonts");
    }

    std::string familyDir = std::string("/fonts/") + family.name;
    if (!Storage.exists(familyDir.c_str())) {
      Storage.mkdir(familyDir.c_str());
    }

    for (currentFileIndex = 0; currentFileIndex < family.files.size(); ++currentFileIndex) {
      const auto& file = family.files[currentFileIndex];
      std::string destPath = familyDir + "/" + file.name;

      currentFileDownloaded = 0;
      currentFileTotal = file.size;
      size_t previousBytesOverallDownloaded = totalBytesDownloaded;
      lastDlPercentage = -1;

      requestUpdateAndWait();

      bool success = downloadFileSecure(file.downloadUrl, destPath, file.size,
                                        [this, previousBytesOverallDownloaded](size_t downloaded, size_t total) {
                                          currentFileDownloaded = downloaded;
                                          currentFileTotal = total;
                                          totalBytesDownloaded = previousBytesOverallDownloaded + downloaded;

                                          if (totalBytesOverall > 0) {
                                            float progress = static_cast<float>(totalBytesDownloaded) /
                                                             static_cast<float>(totalBytesOverall);
                                            int percentage = static_cast<int>(progress * 100);
                                            if (percentage >= lastDlPercentage + 1) {
                                              lastDlPercentage = percentage;
                                              requestUpdate();
                                            }
                                          }
                                        });

      if (!success) {
        errorMessage = "Download failed for " + file.name;
        return false;
      }

      totalBytesDownloaded = previousBytesOverallDownloaded + file.size;
      downloadedFiles++;
    }
  }

  return true;
}

void FontDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DOWNLOAD_FONTS));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == CHECKING_FOR_FONTS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_FETCHING_FONTS));
  } else if (state == CHOOSE_TYPE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top - height * 4, tr(STR_DOWNLOAD_TYPE_TITLE), true, EpdFontFamily::BOLD);

    int y = top - height * 2;
    renderer.drawCenteredText(UI_10_FONT_ID, y, "EPD fonts: fast, cleaner, fixed size.");
    renderer.drawCenteredText(UI_10_FONT_ID, y + height, "TTF/OTF fonts: slower, resizable.");

    y += height * 3;
    std::string preStr = std::string(preconvertedType ? "[x] " : "[ ] ") + tr(STR_DOWNLOAD_TYPE_PRECONVERTED);
    std::string ttfStr = std::string(!preconvertedType ? "[x] " : "[ ] ") + tr(STR_DOWNLOAD_TYPE_TTF);

    renderer.drawCenteredText(UI_10_FONT_ID, y, preStr.c_str(), true,
                              preconvertedType ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    y += height * 1.5;
    renderer.drawCenteredText(UI_10_FONT_ID, y, ttfStr.c_str(), true,
                              !preconvertedType ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "Up", "Down");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == SELECT_FONTS) {
    renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + height, tr(STR_SELECT_FONTS_TITLE), true,
                              EpdFontFamily::BOLD);

    int itemY = top - height;
    int visibleItems = 6;

    for (int i = 0; i < visibleItems && (scrollOffset + i) <= static_cast<int>(fontFamilies.size()); ++i) {
      int index = scrollOffset + i;
      bool isSelected = (index == selectedIndex);

      std::string prefix = isSelected ? "> " : "  ";

      if (index == static_cast<int>(fontFamilies.size())) {
        renderer.drawText(UI_10_FONT_ID, 20, itemY, (prefix + "[ " + tr(STR_DOWNLOAD_START) + " ]").c_str(), true,
                          isSelected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      } else {
        bool isChecked = selectedFamilies[index];
        prefix += isChecked ? "[x] " : "[ ] ";
        renderer.drawText(UI_10_FONT_ID, 20, itemY, (prefix + fontFamilies[index].name).c_str(), true,
                          isSelected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      }
      itemY += height * 1.5;
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "Up", "Down");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top - height * 2, tr(STR_DOWNLOADING));

    if (currentFamilyIndex < fontFamilies.size() && currentFileIndex < fontFamilies[currentFamilyIndex].files.size()) {
      const auto& famName = fontFamilies[currentFamilyIndex].name;
      const auto& fileName = fontFamilies[currentFamilyIndex].files[currentFileIndex].name;
      renderer.drawCenteredText(UI_10_FONT_ID, top - height, (famName + " / " + fileName).c_str());
    }

    int y = top + height + metrics.verticalSpacing;

    float progress = 0;
    if (totalBytesOverall > 0)
      progress = static_cast<float>(totalBytesDownloaded) / static_cast<float>(totalBytesOverall);

    GUI.drawProgressBar(
        renderer,
        Rect(metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight),
        static_cast<int>(progress * 100), 100);

    y += metrics.progressBarHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y, (std::to_string(static_cast<int>(progress * 100)) + "%").c_str());
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_DOWNLOAD_FAILED), true, EpdFontFamily::BOLD);
    if (!errorMessage.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, top + height + metrics.verticalSpacing, errorMessage.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FINISHED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_FONTS_DOWNLOADED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

void FontDownloadActivity::loop() {
  if (state == CHOOSE_TYPE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      preconvertedType = !preconvertedType;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state = CHECKING_FOR_FONTS;
      }
      requestUpdateAndWait();

      if (!fetchFontFamilies()) {
        RenderLock lock(*this);
        state = FAILED;
        errorMessage = "Failed to fetch font list";
        requestUpdate();
        return;
      }

      {
        RenderLock lock(*this);
        state = SELECT_FONTS;
        selectedFamilies.assign(fontFamilies.size(), false);
        selectedIndex = 0;
        scrollOffset = 0;
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == SELECT_FONTS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (selectedIndex > 0) selectedIndex--;
      if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (selectedIndex < static_cast<int>(fontFamilies.size())) selectedIndex++;
      if (selectedIndex >= scrollOffset + 6) scrollOffset++;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (selectedIndex == static_cast<int>(fontFamilies.size())) {
        bool anySelected = false;
        for (bool b : selectedFamilies)
          if (b) {
            anySelected = true;
            break;
          }
        if (!anySelected) {
          // Can show error or ignore
          return;
        }

        {
          RenderLock lock(*this);
          state = DOWNLOADING;
        }
        requestUpdateAndWait();

        if (!downloadFontFiles()) {
          {
            RenderLock lock(*this);
            state = FAILED;
          }
          requestUpdate();
          return;
        }

        {
          RenderLock lock(*this);
          state = FINISHED;
        }
        requestUpdate();
      } else {
        selectedFamilies[selectedIndex] = !selectedFamilies[selectedIndex];
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == FAILED || state == FINISHED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }
}
