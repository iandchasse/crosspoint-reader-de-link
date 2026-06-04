#include "UsbMscMode.h"
#include <Arduino.h>
#include <USB.h>
#include <USBMSC.h>
#include <tusb.h>
#include <soc/usb_serial_jtag_struct.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/soc.h>
#include <SD_MMC.h>
#include <driver/sdmmc_types.h>
#include <sdmmc_cmd.h>

#include "SilentRestart.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"

// External globals from main.cpp
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <GfxRenderer.h>
#include <FontDecompressor.h>
#include <FontCacheManager.h>
#include <HalStorage.h>
#include "fontIds.h"

extern HalDisplay display;
extern HalGPIO gpio;
extern GfxRenderer renderer;
extern FontDecompressor fontDecompressor;
extern FontCacheManager fontCacheManager;
extern EpdFontFamily ui12FontFamily;

class MySDMMCFS : public fs::SDMMCFS {
public:
  sdmmc_card_t* getCard() { return _card; }
};

#define WRITE_BUF_SIZE (32 * 1024)
static uint8_t* write_back_buf = nullptr;
static uint32_t write_back_lba = 0;
static uint32_t write_back_sectors = 0;
static SemaphoreHandle_t write_sem = nullptr;
static TaskHandle_t write_task_handle = nullptr;
static volatile bool write_pending = false;
static volatile bool write_success = true;

void wait_for_pending_write() {
  if (write_pending) {
    xSemaphoreTake(write_sem, portMAX_DELAY);
    write_pending = false;
  }
}

void sdmmc_write_task(void* pvParameters) {
  sdmmc_card_t* card = (sdmmc_card_t*)pvParameters;
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    esp_err_t err = sdmmc_write_sectors(card, write_back_buf, write_back_lba, write_back_sectors);
    write_success = (err == ESP_OK);
    xSemaphoreGive(write_sem);
  }
}

void forceUsbSerialJtag() {
  // Clear software override for USB PHY selection (revert to hardware/eFuse default which is USB_SERIAL_JTAG)
  CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL);
  // Clear software selection bit (just in case, 0 selects USB_SERIAL_JTAG)
  CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_USB_PHY_SEL);

  // Route USB PHY back to USB_SERIAL_JTAG controller and restore its default registers
  USB_SERIAL_JTAG.conf0.phy_sel = 0;
  USB_SERIAL_JTAG.conf0.pad_pull_override = 0;
  USB_SERIAL_JTAG.conf0.dp_pullup = 1;
  USB_SERIAL_JTAG.conf0.usb_pad_enable = 1;
}

void restartToNormalMode() {
  // 1. Detach TinyUSB device if active to signal soft disconnect to host
  if (tud_inited()) {
    tud_disconnect();
    delay(100);
  }

  // 2. Disable USB PHY control and internal pull-ups on the shared pins
  // to release them to the GPIO matrix
  USB_SERIAL_JTAG.conf0.usb_pad_enable = 0;
  USB_SERIAL_JTAG.conf0.dp_pullup = 0;
  USB_SERIAL_JTAG.conf0.pad_pull_override = 1;
  delay(10);

  // 3. Force physical USB disconnect on host by pulling D+ and D- low
  pinMode(19, OUTPUT);
  pinMode(20, OUTPUT);
  digitalWrite(19, LOW);
  digitalWrite(20, LOW);
  delay(500);

  // 4. Force JTAG/Serial PHY routing and registers
  forceUsbSerialJtag();
  delay(100);

  ESP.restart();
}

void runUsbMscMode() {
  usbMscBootFlag = 0;

  gpio.begin();
  display.begin(false);
  renderer.begin();

  if (!fontDecompressor.init()) {
    // Log error, but continue
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);

  const auto width = renderer.getScreenWidth();
  const auto height = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, height / 2 - 40, "USB Mass Storage Mode", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, height / 2, "Connecting as USB Drive...", true, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(UI_12_FONT_ID, height / 2 + 40, "Hold Power Button for 5s to exit", true,
                            EpdFontFamily::REGULAR);
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);

  if (!Storage.begin()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, height / 2, "SD Card Init Failed!", true, EpdFontFamily::BOLD);
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    delay(5000);
    restartToNormalMode();
  }

  sdmmc_card_t* card = ((MySDMMCFS*)&SD_MMC)->getCard();
  if (card != nullptr) {
    if (write_back_buf == nullptr) {
      write_back_buf = (uint8_t*)heap_caps_malloc(WRITE_BUF_SIZE, MALLOC_CAP_DMA);
    }
    if (write_sem == nullptr) {
      write_sem = xSemaphoreCreateBinary();
    }
    if (write_task_handle == nullptr) {
      xTaskCreate(sdmmc_write_task, "sd_write_task", 4096, card, 5, &write_task_handle);
    }
  }

  static USBMSC msc;
  msc.vendorID("ESP32S3");
  msc.productID("EPDReader");
  msc.productRevision("1.0");

  msc.onRead([](uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) -> int32_t {
    sdmmc_card_t* card = ((MySDMMCFS*)&SD_MMC)->getCard();
    if (!card) return -1;
    uint32_t secSize = card->csd.sector_size;
    if (!secSize) return -1;
    uint32_t sectors = bufsize / secSize;
    if (sectors == 0) return 0;

    wait_for_pending_write();
    if (!write_success) {
      write_success = true;
      return -1;
    }

    esp_err_t err = sdmmc_read_sectors(card, buffer, lba, sectors);
    if (err != ESP_OK) return -1;
    return bufsize;
  });

  msc.onWrite([](uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) -> int32_t {
    sdmmc_card_t* card = ((MySDMMCFS*)&SD_MMC)->getCard();
    if (!card) return -1;
    uint32_t secSize = card->csd.sector_size;
    if (!secSize) return -1;
    uint32_t sectors = bufsize / secSize;
    if (sectors == 0) return 0;

    wait_for_pending_write();
    if (!write_success) {
      write_success = true;
      return -1;
    }

    if (bufsize <= WRITE_BUF_SIZE && write_back_buf != nullptr) {
      memcpy(write_back_buf, buffer, bufsize);
      write_back_lba = lba;
      write_back_sectors = sectors;
      write_pending = true;
      xTaskNotifyGive(write_task_handle);
      return bufsize;
    } else {
      esp_err_t err = sdmmc_write_sectors(card, buffer, lba, sectors);
      if (err != ESP_OK) return -1;
      return bufsize;
    }
  });

  msc.onStartStop([](uint8_t power_condition, bool start, bool load_eject) -> bool {
    wait_for_pending_write();
    return true;
  });

  msc.mediaPresent(true);
  msc.begin(SD_MMC.numSectors(), SD_MMC.sectorSize());

  USB.begin();

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, height / 2 - 40, "USB Mass Storage Mode", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, height / 2, "Connected as USB Drive", true, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(UI_12_FONT_ID, height / 2 + 40, "Hold Power Button for 5s to exit", true,
                            EpdFontFamily::REGULAR);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);

  unsigned long powerBtnPressedStart = 0;
  while (true) {
    gpio.update();
    if (gpio.isPressed(HalGPIO::BTN_POWER)) {
      if (powerBtnPressedStart == 0) {
        powerBtnPressedStart = millis();
      } else if (millis() - powerBtnPressedStart >= 5000) {
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, height / 2, "Restarting...", true, EpdFontFamily::BOLD);
        renderer.displayBuffer(HalDisplay::FULL_REFRESH);
        delay(1000);
        wait_for_pending_write(); // Ensure all background writes complete before restart
        restartToNormalMode();
      }
    } else {
      powerBtnPressedStart = 0;
    }
    delay(50);
  }
}
