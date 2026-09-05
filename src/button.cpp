#include "button.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp32-hal-tinyusb.h>

#include "hardware.h"
#include "trmnl_keys.h"

static constexpr int BUTTON_HOLD_TIME = 5000;
static constexpr int BUTTON_FACTORY_RESET = 15000;

extern Preferences prefs;
extern bool forceSpecialFunctionNextBoot;
extern void deviceLog(const char* fmt, ...);
extern void showErrorScreen(const String& message);
extern void showSetupScreen(const String& message);

static void handleKeyPress(WakePress press, bool restartForClick) {
  if (press == WakePress::LONGEST) {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.clear();
    prefs.end();
    showErrorScreen("Factory Reset\n\nAll settings cleared\nRestarting...");
    delay(1500);
    ESP.restart();
  }

  if (press == WakePress::LONG) {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.remove(KEY_WIFI_SSID);
    prefs.remove(KEY_WIFI_PASS);
    prefs.putInt(KEY_WIFI_RETRY_COUNT, 1);
    prefs.end();
    showSetupScreen("WiFi cleared\n\nRestarting...");
    delay(1000);
    ESP.restart();
  }

  if (restartForClick) {
    forceSpecialFunctionNextBoot = true;
    ESP.restart();
  }
}

void enterFlashMode() {
  deviceLog("BOOT: entering ROM download mode\n");
  Serial.flush();
  delay(100);
  usb_persist_restart(RESTART_BOOTLOADER);
  while (true) delay(1000);
}

bool handleKeyButtonAtStartup() {
  pinMode(BUTTON_KEY_PIN, INPUT_PULLUP);
  if (digitalRead(BUTTON_KEY_PIN) == LOW) {
    handleKeyPress(detectKeyButtonPress(), true);
    return true;
  }
  return false;
}

void checkRuntimeButtons() {
  pinMode(BUTTON_BOOT_PIN, INPUT_PULLUP);
  pinMode(BUTTON_KEY_PIN, INPUT_PULLUP);
  if (digitalRead(BUTTON_BOOT_PIN) == LOW) enterFlashMode();
  if (digitalRead(BUTTON_KEY_PIN) == LOW) {
    handleKeyPress(detectKeyButtonPress(), true);
  }
}

WakePress detectKeyButtonPress() {
  pinMode(BUTTON_KEY_PIN, INPUT_PULLUP);
  if (digitalRead(BUTTON_KEY_PIN) == HIGH) return WakePress::CLICK;

  unsigned long start = millis();
  while (digitalRead(BUTTON_KEY_PIN) == LOW) {
    if (millis() - start >= BUTTON_FACTORY_RESET) break;
    delay(10);
  }
  unsigned long held = millis() - start;

  if (held >= BUTTON_FACTORY_RESET) return WakePress::LONGEST;
  if (held >= BUTTON_HOLD_TIME) return WakePress::LONG;
  return WakePress::CLICK;
}
