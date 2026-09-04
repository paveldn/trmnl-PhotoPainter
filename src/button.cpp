#include "button.h"

#include <Arduino.h>
#include <Preferences.h>

#include "hardware.h"
#include "trmnl_keys.h"

static constexpr int BUTTON_MEDIUM_TIME = 1000;
static constexpr int BUTTON_HOLD_TIME = 6000;
static constexpr int BUTTON_FACTORY_RESET = 16000;

extern Preferences prefs;
extern void deviceLog(const char* fmt, ...);
extern void showErrorScreen(const String& message);
extern void showSetupScreen(const String& message);

bool handleBootButtonReset() {
  pinMode(BUTTON_BOOT_PIN, INPUT_PULLUP);
  if (digitalRead(BUTTON_BOOT_PIN) == LOW) {
    deviceLog("Button held at boot\n");
    unsigned long pressStart = millis();
    while (digitalRead(BUTTON_BOOT_PIN) == LOW) {
      if (millis() - pressStart > BUTTON_FACTORY_RESET) break;
      delay(50);
    }
    unsigned long holdTime = millis() - pressStart;

    if (holdTime >= BUTTON_FACTORY_RESET) {
      prefs.begin(NVS_NAMESPACE, false);
      prefs.clear();
      prefs.end();
      showErrorScreen("Factory Reset\n\nAll settings cleared\nRestarting...");
      delay(2000);
      ESP.restart();
      return true;
    }

    if (holdTime >= BUTTON_HOLD_TIME) {
      prefs.begin(NVS_NAMESPACE, false);
      prefs.remove(KEY_WIFI_SSID);
      prefs.remove(KEY_WIFI_PASS);
      prefs.putInt(KEY_WIFI_RETRY_COUNT, 1);
      prefs.end();
    }
  }
  return false;
}

void checkRuntimeReset() {
  pinMode(BUTTON_BOOT_PIN, INPUT_PULLUP);
  if (digitalRead(BUTTON_BOOT_PIN) == LOW) {
    unsigned long start = millis();
    while (digitalRead(BUTTON_BOOT_PIN) == LOW) {
      unsigned long held = millis() - start;
      if (held >= BUTTON_FACTORY_RESET) {
        prefs.begin(NVS_NAMESPACE, false);
        prefs.clear();
        prefs.end();
        showErrorScreen("Factory Reset\n\nAll settings cleared\nRestarting...");
        delay(1500);
        ESP.restart();
        return;
      }
      delay(50);
    }
    if (millis() - start >= BUTTON_HOLD_TIME) {
      prefs.begin(NVS_NAMESPACE, false);
      prefs.remove(KEY_WIFI_SSID);
      prefs.remove(KEY_WIFI_PASS);
      prefs.putInt(KEY_WIFI_RETRY_COUNT, 1);
      prefs.end();
      showSetupScreen("WiFi cleared\n\nRestarting...");
      delay(1000);
      ESP.restart();
    }
  }
}

WakePress detectButtonWakePress() {
  pinMode(BUTTON_BOOT_PIN, INPUT_PULLUP);
  if (digitalRead(BUTTON_BOOT_PIN) == HIGH) return WakePress::CLICK;

  unsigned long start = millis();
  while (digitalRead(BUTTON_BOOT_PIN) == LOW) {
    if (millis() - start >= BUTTON_FACTORY_RESET) break;
    delay(10);
  }
  unsigned long held = millis() - start;

  if (held >= BUTTON_FACTORY_RESET) return WakePress::LONGEST;
  if (held >= BUTTON_HOLD_TIME) return WakePress::LONG;
  if (held >= BUTTON_MEDIUM_TIME) return WakePress::MEDIUM;
  return WakePress::CLICK;
}

