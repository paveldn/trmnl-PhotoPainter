#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

#include "api_client.h"
#include "button.h"
#include "captive_portal.h"
#include "display.h"
#include "image_pipeline.h"
#include "ota.h"
#include "power.h"
#include "preferences_persistence.h"
#include "trmnl_keys.h"
#include "wifi_network.h"

#ifndef FW_VERSION
#error "FW_VERSION must be defined by platformio.ini"
#endif

#ifndef DEVICE_MODEL
#define DEVICE_MODEL "photopainter"
#endif

static constexpr int DEFAULT_REFRESH_RATE = 900;
static constexpr int WIFI_AP_TIMEOUT = 300;
static constexpr int LOG_BUFFER_SIZE = 4096;
static constexpr float LOW_BATTERY_VOLTAGE = 3.4f;

RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR uint8_t savedBSSID[6] = {0};
RTC_DATA_ATTR uint8_t savedChannel = 0;
RTC_DATA_ATTR int wifiFailCount = 0;
RTC_DATA_ATTR int lastWakeTime = 0;

Preferences prefs;
unsigned long startupMillis = 0;
String configuredSSID;
String configuredPass;
String apiKey;
String apiBaseUrl;
String friendlyId;
int refreshRate = DEFAULT_REFRESH_RATE;
bool forceOtaOnThisBoot = false;
bool otaEnabled = true;
bool otaBetaMode = false;

#if defined(DEBUG_LOGS) || defined(ENABLE_SERVER_LOGS)
String logBuffer;
#endif

void deviceLog(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
#ifdef DEBUG_LOGS
  Serial.print(buf);
#endif
#ifdef ENABLE_SERVER_LOGS
  if (logBuffer.length() < LOG_BUFFER_SIZE) {
    logBuffer += buf;
  }
#endif
}

void disableWiFiPS() {
  esp_wifi_set_ps(WIFI_PS_NONE);
}

void enableWiFiPS() {
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

const char* FW_VERSION_STR = FW_VERSION;
int DEFAULT_REFRESH_RATE_VAL = DEFAULT_REFRESH_RATE;
int WIFI_AP_TIMEOUT_VAL = WIFI_AP_TIMEOUT;
const char* DEFAULT_API_BASE_URL_STR = "https://trmnl.app";
const char* UPDATE_SOURCE_STR = "COLD";

void invalidateImageCache(const char* reason = nullptr) {
  prefs.begin(NVS_NAMESPACE, false);
  bool hadFilename = prefs.isKey(KEY_LAST_FILENAME);
  bool hadEtag = prefs.isKey(KEY_IMAGE_ETAG);
  bool hadLastMod = prefs.isKey(KEY_IMAGE_LASTMOD);
  prefs.remove(KEY_LAST_FILENAME);
  prefs.remove(KEY_IMAGE_ETAG);
  prefs.remove(KEY_IMAGE_LASTMOD);
  prefs.end();

  if ((hadFilename || hadEtag || hadLastMod) && reason && strlen(reason) > 0) {
    deviceLog("Image cache invalidated (%s)\n", reason);
  }
}

void setup() {
#ifdef DEBUG_LOGS
  Serial.begin(115200);
  delay(300);
#endif
#if defined(DEBUG_LOGS) || defined(ENABLE_SERVER_LOGS)
  logBuffer.reserve(LOG_BUFFER_SIZE);
#endif

  bootCount++;
  startupMillis = millis();
  esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
  const char* wakeStr = "COLD";
  if (wakeup == ESP_SLEEP_WAKEUP_TIMER) wakeStr = "TIMER";
  else if (wakeup == ESP_SLEEP_WAKEUP_EXT1) wakeStr = "EXT1";
  UPDATE_SOURCE_STR = wakeStr;
  bool coldBoot = (wakeup == ESP_SLEEP_WAKEUP_UNDEFINED);

#ifdef FORCE_OTA_ON_NEXT_BOOT
  forceOtaOnThisBoot = coldBoot;
#endif

  setCpuFrequencyMhz(80);
  btStop();
  initPower();
  initDisplay();
  deviceLog("[Boot #%d] Wake: %s\n", bootCount, wakeStr);

  loadSettings();

  bool isSpecialFunction = false;
  if (wakeup == ESP_SLEEP_WAKEUP_EXT1) {
    WakePress press = detectButtonWakePress();
    switch (press) {
      case WakePress::LONGEST:
        prefs.begin(NVS_NAMESPACE, false);
        prefs.clear();
        prefs.end();
        showErrorScreen("Factory Reset\n\nAll settings cleared\nRestarting...");
        delay(2000);
        ESP.restart();
        return;
      case WakePress::LONG:
        prefs.begin(NVS_NAMESPACE, false);
        prefs.remove(KEY_WIFI_SSID);
        prefs.remove(KEY_WIFI_PASS);
        prefs.putInt(KEY_WIFI_RETRY_COUNT, 1);
        prefs.end();
        showSetupScreen("WiFi cleared\n\nRestarting...");
        delay(1000);
        ESP.restart();
        return;
      case WakePress::MEDIUM:
        if (specialFunction == "add_wifi") {
          showSetupScreen("Opening WiFi Setup...\nPhotoPainter-TRMNL\n192.168.4.1");
          startCaptivePortal();
          return;
        }
        isSpecialFunction = true;
        break;
      case WakePress::CLICK:
      default:
        break;
    }
  } else if (handleBootButtonReset()) {
    return;
  }

  float bootVoltage = getBatteryVoltage();
  bool externalPower = isExternalPowerPresent();
  if (bootVoltage > 0.5f && bootVoltage < LOW_BATTERY_VOLTAGE && !externalPower) {
    showLowBatteryAndShutdown();
    return;
  }

  if (configuredSSID.length() == 0) {
    showSetupScreen("Connect to WiFi:\nPhotoPainter-TRMNL\nThen open: 192.168.4.1");
    startCaptivePortal();
    return;
  }

  if (!connectWiFi()) {
    wifiErrorSleep();
    return;
  }

  prefs.begin(NVS_NAMESPACE, false);
  prefs.putInt(KEY_WIFI_RETRY_COUNT, 1);
  prefs.end();

  if (apiKey.length() == 0) {
    registerDevice();
    if (apiKey.length() == 0) return;
  }

  fetchAndDisplay(getBatteryVoltage(), isSpecialFunction);
  goToDeepSleep(refreshRate);
}

void loop() {
  goToDeepSleep(refreshRate > 0 ? refreshRate : DEFAULT_REFRESH_RATE);
}
