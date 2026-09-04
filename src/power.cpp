#include "power.h"

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <algorithm>
#include <cmath>

#include "display.h"
#include "hardware.h"

static constexpr float LOW_BATTERY_VOLTAGE = 3.4f;
static XPowersPMU pmu;
static bool pmuReady = false;

extern unsigned long startupMillis;
extern int lastWakeTime;
extern void deviceLog(const char* fmt, ...);
extern void sendLogs();
extern void invalidateImageCache(const char* reason);

void initPower() {
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  digitalWrite(LED_RED_PIN, HIGH);
  digitalWrite(LED_GREEN_PIN, HIGH);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  pmuReady = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA_PIN, I2C_SCL_PIN);
  if (!pmuReady) {
    deviceLog("AXP2101 init failed\n");
    return;
  }

  pmu.setALDO3Voltage(3300);
  pmu.enableALDO3();
  pmu.setALDO4Voltage(3300);
  pmu.enableALDO4();
  pmu.enableBattVoltageMeasure();
  pmu.enableVbusVoltageMeasure();
  pmu.enableBattDetection();
}

float readBatteryAvg(int samples, int delayMs) {
  if (!pmuReady) return 0.0f;
  samples = std::clamp(samples, 1, 32);
  float sum = 0.0f;
  float minV = 100.0f;
  float maxV = 0.0f;
  for (int i = 0; i < samples; ++i) {
    float v = pmu.getBattVoltage() / 1000.0f;
    sum += v;
    minV = min(minV, v);
    maxV = max(maxV, v);
    if (i < samples - 1) delay(delayMs);
  }
  if (samples >= 5) return (sum - minV - maxV) / (samples - 2);
  return sum / samples;
}

float getBatteryVoltage() {
  float voltage = readBatteryAvg(8, 30);
  if (!isExternalPowerPresent() && voltage > 0.1f) voltage -= 0.06f;
  deviceLog("Bat: %.2fV\n", voltage);
  return voltage;
}

bool isExternalPowerPresent() {
  if (!pmuReady) return false;
  return pmu.getVbusVoltage() > 4000;
}

bool isBatteryCharging() {
  if (!pmuReady) return false;
  return pmu.isCharging();
}

void showLowBatteryAndShutdown() {
  invalidateImageCache("low_battery_screen");
  display.clear(PP_WHITE);
  display.setTextColor(PP_BLACK);
  display.setTextSize(3);
  display.setCursor(280, 190);
  display.print("LOW BATTERY");
  display.setTextSize(2);
  display.setCursor(235, 245);
  display.print("Connect USB to charge");
  display.refresh();

  deviceLog("Shutting down on low battery\n");
  sendLogs();
  Serial.flush();
  display.sleep();
  delay(100);
  esp_deep_sleep_start();
}

void goToDeepSleep(int seconds) {
  if (seconds < 15) seconds = 16;
  lastWakeTime = (millis() - startupMillis) / 1000;
  deviceLog("Sleep: %d seconds\n", seconds);
  sendLogs();
  Serial.flush();
  delay(10);

  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
    delay(10);
  }

  display.sleep();
  esp_sleep_enable_ext1_wakeup((1ULL << BUTTON_BOOT_PIN), ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep(static_cast<uint64_t>(seconds) * 1000000ULL);
}
