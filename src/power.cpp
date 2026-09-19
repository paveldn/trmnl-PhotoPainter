#include "power.h"

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <algorithm>
#include <cmath>

#include "display.h"
#include "button.h"
#include "hardware.h"

static constexpr float LOW_BATTERY_VOLTAGE = 3.4f;
static XPowersPMU pmu;
static bool pmuReady = false;

extern unsigned long startupMillis;
extern int lastWakeTime;
extern void deviceLog(const char* fmt, ...);
extern void sendLogs();
extern void invalidateImageCache(const char* reason);

static void wakePmu() {
  // AXP2101 turns its I2C interface off in PMIC sleep. Waveshare's reference
  // firmware wakes it by holding IRQ low for more than 16 ms before I2C init.
  pinMode(AXP2101_IRQ_PIN, OUTPUT);
  digitalWrite(AXP2101_IRQ_PIN, LOW);
  delay(100);
  digitalWrite(AXP2101_IRQ_PIN, HIGH);
  delay(200);
  pinMode(AXP2101_IRQ_PIN, INPUT_PULLUP);
}

static void recoverI2cBus() {
  // A slave interrupted by deep sleep can hold SDA low. Clock out a pending
  // byte and generate STOP before handing the pins to Wire.
  pinMode(I2C_SCL_PIN, OUTPUT_OPEN_DRAIN);
  pinMode(I2C_SDA_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(I2C_SCL_PIN, HIGH);
  digitalWrite(I2C_SDA_PIN, HIGH);
  delayMicroseconds(5);

  for (int i = 0; i < 9; ++i) {
    digitalWrite(I2C_SCL_PIN, LOW);
    delayMicroseconds(5);
    digitalWrite(I2C_SCL_PIN, HIGH);
    delayMicroseconds(5);
  }

  digitalWrite(I2C_SDA_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(I2C_SCL_PIN, HIGH);
  delayMicroseconds(5);
  digitalWrite(I2C_SDA_PIN, HIGH);
  delayMicroseconds(5);

  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
}

static void stopPmuMeasurements() {
  if (!pmuReady) return;

  pmu.disableBattVoltageMeasure();
  pmu.disableVbusVoltageMeasure();
  pmu.disableBattDetection();
}

static void disableDisplayRails() {
  if (!pmuReady) return;
  pmu.disableALDO4();
  pmu.disableALDO3();
}

static bool preparePmuForSleep() {
  if (!pmuReady) return false;

  pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  pmu.clearIrqStatus();

  int sleepControl = pmu.readRegister(0x26);
  if (sleepControl < 0) {
    deviceLog("AXP2101 sleep control read failed\n");
    return false;
  }

  // Restore the pre-sleep regulator configuration when IRQ wakes the PMIC,
  // do not pull PWROK low, and accept IRQ-low as the wake event.
  if (!(sleepControl & 0x04)) {
    pmu.wakeupControl(XPOWERS_AXP2101_WAKEUP_DC_DLO_SELECT, true);
  }
  if (sleepControl & 0x08) {
    pmu.wakeupControl(XPOWERS_AXP2101_WAKEUP_PWROK_TO_LOW, false);
  }
  if (!(sleepControl & 0x10)) {
    pmu.wakeupControl(XPOWERS_AXP2101_WAKEUP_IRQ_PIN_TO_LOW, true);
  }

  stopPmuMeasurements();
  if (!pmu.enableSleep()) {
    deviceLog("AXP2101 sleep enable failed\n");
    return false;
  }

  // These two outputs are tied together as EPD_VCC on the schematic. DCDC1,
  // which powers the ESP32 and KEY wake circuitry, remains enabled.
  disableDisplayRails();
  return true;
}

static void holdSleepGpios() {
  digitalWrite(LED_RED_PIN, HIGH);
  digitalWrite(LED_GREEN_PIN, HIGH);
  gpio_hold_en(static_cast<gpio_num_t>(LED_RED_PIN));
  gpio_hold_en(static_cast<gpio_num_t>(LED_GREEN_PIN));
  gpio_deep_sleep_hold_en();
}

void disconnectWiFi() {
  if (WiFi.getMode() == WIFI_OFF) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(10);
}

void initPower() {
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(static_cast<gpio_num_t>(LED_RED_PIN));
  gpio_hold_dis(static_cast<gpio_num_t>(LED_GREEN_PIN));

  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  digitalWrite(LED_RED_PIN, HIGH);
  digitalWrite(LED_GREEN_PIN, HIGH);

  wakePmu();
  recoverI2cBus();
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 100000);
  pmuReady = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA_PIN, I2C_SCL_PIN);
  if (!pmuReady) {
    deviceLog("AXP2101 init failed\n");
    return;
  }

  pmu.setALDO3Voltage(3300);
  pmu.enableALDO3();
  pmu.setALDO4Voltage(3300);
  pmu.enableALDO4();
  // Match the conservative settling interval used by the working reference.
  delay(500);
  pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_2000MA);
  pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
  pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_500MA);
  pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
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
  if (pmu.isVbusIn() || pmu.isVbusGood()) return true;

  int aboveThreshold = 0;
  for (int i = 0; i < 3; ++i) {
    if (pmu.getVbusVoltage() > 4000) ++aboveThreshold;
    if (i < 2) delay(5);
  }
  return aboveThreshold >= 2;
}

bool isBatteryCharging() {
  if (!pmuReady) return false;
  return pmu.isBatteryConnect() && pmu.isCharging();
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
  stopPmuMeasurements();
  disableDisplayRails();
  Wire.end();
  holdSleepGpios();
  delay(100);
  esp_sleep_pd_config(ESP_PD_DOMAIN_MAX, ESP_PD_OPTION_AUTO);
  esp_deep_sleep_start();
}

void goToDeepSleep(int seconds) {
  if (seconds < 15) seconds = 16;
  lastWakeTime = (millis() - startupMillis) / 1000;
  deviceLog("Sleep: %d seconds\n", seconds);
  // A changed-image path sends logs and turns Wi-Fi off before the slow
  // physical panel refresh. Do not attempt another HTTP request afterward.
  if (WiFi.status() == WL_CONNECTED) sendLogs();
  Serial.flush();
  delay(10);

  disconnectWiFi();

  display.sleep();

  // Keep native USB alive on external power so esptool can reset the device
  // into its ROM loader without the PhotoPainter's BOOT/PWR button sequence.
  if (isExternalPowerPresent()) {
    deviceLog("USB power: staying available for flashing\n");
    int remaining = seconds;
    while (remaining > 0 && isExternalPowerPresent()) {
      for (int i = 0; i < 20; ++i) {
        checkRuntimeButtons();
        delay(50);
      }
      --remaining;
    }
    if (isExternalPowerPresent()) {
      ESP.restart();
    }
    seconds = max(remaining, 16);
    deviceLog("USB removed: sleeping for %d seconds\n", seconds);
  }

  pinMode(BUTTON_KEY_PIN, INPUT_PULLUP);

  // GPIO5 is AXP2101 SYS_OUT on this board, not the physical PWR button.
  // SYS_OUT changes state during a USB-to-battery transition and must never
  // be used as an ESP32 wake source. The PMIC handles the PWR button itself.
  // BOOT/GPIO0 is intentionally excluded too: a false low would put the
  // device into the persistent ROM downloader with no way to recover on
  // battery. BOOT remains available while the firmware is awake on USB.
  rtc_gpio_pullup_en(static_cast<gpio_num_t>(BUTTON_KEY_PIN));
  rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(BUTTON_KEY_PIN));

  // Never arm an already-low KEY: it would wake the ESP32 immediately and
  // create a continuous fetch/display loop. The timer remains armed even if
  // KEY is held during sleep entry.
  if (digitalRead(BUTTON_KEY_PIN) == HIGH) {
    esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_KEY_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
  }
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);

  // Configure both ESP32 wake sources before putting the PMIC to sleep. On
  // wake, wakePmu() restores its outputs before the first I2C transaction.
  preparePmuForSleep();
  Wire.end();
  holdSleepGpios();
  esp_sleep_pd_config(ESP_PD_DOMAIN_MAX, ESP_PD_OPTION_AUTO);
  esp_deep_sleep_start();
}
