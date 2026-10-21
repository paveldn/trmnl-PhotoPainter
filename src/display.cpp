#include "display.h"

#include <SPI.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>

#include "power.h"

namespace {

constexpr unsigned long EPD_BUSY_TIMEOUT_MS = 120000UL;

class ScopedCpuFrequency {
 public:
  explicit ScopedCpuFrequency(uint32_t activeMhz)
      : restoreMhz_(getCpuFrequencyMhz()) {
    setCpuFrequencyMhz(activeMhz);
  }

  ~ScopedCpuFrequency() {
    if (getCpuFrequencyMhz() != restoreMhz_) {
      setCpuFrequencyMhz(restoreMhz_);
    }
  }

  ScopedCpuFrequency(const ScopedCpuFrequency&) = delete;
  ScopedCpuFrequency& operator=(const ScopedCpuFrequency&) = delete;

 private:
  uint32_t restoreMhz_;
};

void disableEpdLightSleepWake() {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  gpio_wakeup_disable(static_cast<gpio_num_t>(EPD_BUSY_PIN));
}

}  // namespace

PhotoPainterDisplay display;

extern const char* FW_VERSION_STR;
extern void deviceLog(const char* fmt, ...);
extern void invalidateImageCache(const char* reason);

PhotoPainterDisplay::PhotoPainterDisplay()
    : Adafruit_GFX(DISPLAY_WIDTH, DISPLAY_HEIGHT), buffer_(nullptr), begun_(false) {}

bool PhotoPainterDisplay::begin() {
  if (begun_) return true;

  const size_t bytes = framebufferSize();
  buffer_ = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buffer_) {
    buffer_ = static_cast<uint8_t*>(malloc(bytes));
  }
  if (!buffer_) return false;

  pinMode(EPD_CS_PIN, OUTPUT);
  pinMode(EPD_DC_PIN, OUTPUT);
  pinMode(EPD_RST_PIN, OUTPUT);
  pinMode(EPD_BUSY_PIN, INPUT_PULLUP);
  digitalWrite(EPD_CS_PIN, HIGH);
  digitalWrite(EPD_RST_PIN, HIGH);

  SPI.begin(EPD_SCK_PIN, -1, EPD_MOSI_PIN, EPD_CS_PIN);
  SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));

  clear();
  resetPanel();
  waitBusy();
  delay(50);

  sendCommand(0xAA);
  sendData(0x49);
  sendData(0x55);
  sendData(0x20);
  sendData(0x08);
  sendData(0x09);
  sendData(0x18);

  sendCommand(0x01);
  sendData(0x3F);

  sendCommand(0x00);
  sendData(0x5F);
  sendData(0x69);

  sendCommand(0x03);
  sendData(0x00);
  sendData(0x54);
  sendData(0x00);
  sendData(0x44);

  sendCommand(0x05);
  sendData(0x40);
  sendData(0x1F);
  sendData(0x1F);
  sendData(0x2C);

  sendCommand(0x06);
  sendData(0x6F);
  sendData(0x1F);
  sendData(0x17);
  sendData(0x49);

  sendCommand(0x08);
  sendData(0x6F);
  sendData(0x1F);
  sendData(0x1F);
  sendData(0x22);

  sendCommand(0x30);
  sendData(0x03);

  sendCommand(0x50);
  sendData(0x3F);

  sendCommand(0x60);
  sendData(0x02);
  sendData(0x00);

  sendCommand(0x61);
  sendData(0x03);
  sendData(0x20);
  sendData(0x01);
  sendData(0xE0);

  sendCommand(0x84);
  sendData(0x01);

  sendCommand(0xE3);
  sendData(0x2F);

  sendCommand(0x04);
  waitBusy();
  SPI.endTransaction();
  begun_ = true;
  return true;
}

void PhotoPainterDisplay::clear(uint8_t color) {
  if (!buffer_) return;
  memset(buffer_, (color << 4) | (color & 0x0F), framebufferSize());
}

void PhotoPainterDisplay::refresh() {
  if (!begun_ && !begin()) return;
  SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
  sendCommand(0x10);
  sendBuffer(buffer_, framebufferSize());

  sendCommand(0x04);
  waitBusy();

  sendCommand(0x06);
  sendData(0x6F);
  sendData(0x1F);
  sendData(0x17);
  sendData(0x49);

  sendCommand(0x12);
  sendData(0x00);

  // No SPI transfer is active while the panel performs its long physical
  // refresh. Releasing the transaction also makes CPU clock changes safe if
  // the Light-sleep path has to fall back to reduced-clock polling.
  SPI.endTransaction();
  waitBusy(true);
  SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));

  sendCommand(0x02);
  sendData(0x00);
  waitBusy();
  SPI.endTransaction();
}

void PhotoPainterDisplay::sleep() {
  if (!begun_) return;
  SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
  sendCommand(0x02);
  sendData(0x00);
  waitBusy();
  SPI.endTransaction();
}

uint8_t* PhotoPainterDisplay::framebuffer() {
  return buffer_;
}

size_t PhotoPainterDisplay::framebufferSize() const {
  return DISPLAY_WIDTH * DISPLAY_HEIGHT / 2;
}

void PhotoPainterDisplay::drawPixel(int16_t x, int16_t y, uint16_t color) {
  drawNativePixel(x, y, mapGfxColor(color));
}

void PhotoPainterDisplay::drawNativePixel(int16_t x, int16_t y, uint8_t color) {
  if (!buffer_ || x < 0 || y < 0 || x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) return;

  int16_t panelX = DISPLAY_WIDTH - 1 - x;
  int16_t panelY = DISPLAY_HEIGHT - 1 - y;
  size_t index = (static_cast<size_t>(panelY) * DISPLAY_WIDTH + panelX) / 2;
  if ((panelX & 1) == 0) {
    buffer_[index] = (buffer_[index] & 0x0F) | ((color & 0x0F) << 4);
  } else {
    buffer_[index] = (buffer_[index] & 0xF0) | (color & 0x0F);
  }
}

void PhotoPainterDisplay::resetPanel() {
  digitalWrite(EPD_RST_PIN, HIGH);
  delay(50);
  digitalWrite(EPD_RST_PIN, LOW);
  delay(20);
  digitalWrite(EPD_RST_PIN, HIGH);
  delay(50);
}

void PhotoPainterDisplay::waitBusyPolling(unsigned long started) {
  // This path is normally used for short panel initialization/power waits.
  // If Light-sleep setup fails, 20 MHz limits the cost of the long wait while
  // still preserving the original polling behavior and timeout.
  ScopedCpuFrequency pollingFrequency(20);
  while (digitalRead(EPD_BUSY_PIN) == LOW) {
    if (millis() - started > EPD_BUSY_TIMEOUT_MS) {
      deviceLog("EPD busy timeout\n");
      return;
    }
    delay(10);
  }
}

void PhotoPainterDisplay::waitBusy(bool allowLightSleep) {
  const unsigned long started = millis();

  // GPIO Light-sleep wake is deliberately limited to battery operation after
  // Wi-Fi is fully off. USB operation retains normal polling so native USB,
  // runtime buttons, and flashing behavior remain unchanged.
  if (!allowLightSleep || isExternalPowerPresent() || WiFi.getMode() != WIFI_OFF) {
    while (digitalRead(EPD_BUSY_PIN) == LOW) {
      if (millis() - started > EPD_BUSY_TIMEOUT_MS) {
        deviceLog("EPD busy timeout\n");
        return;
      }
      delay(10);
    }
    return;
  }

  if (digitalRead(EPD_BUSY_PIN) == HIGH) return;

  const gpio_num_t busyPin = static_cast<gpio_num_t>(EPD_BUSY_PIN);
  esp_err_t gpioResult = gpio_wakeup_enable(busyPin, GPIO_INTR_HIGH_LEVEL);
  esp_err_t wakeResult = gpioResult == ESP_OK
                             ? esp_sleep_enable_gpio_wakeup()
                             : gpioResult;
  unsigned long elapsed = millis() - started;
  uint64_t remainingUs = elapsed < EPD_BUSY_TIMEOUT_MS
                             ? static_cast<uint64_t>(EPD_BUSY_TIMEOUT_MS - elapsed) * 1000ULL
                             : 1ULL;
  esp_err_t timerResult = wakeResult == ESP_OK
                              ? esp_sleep_enable_timer_wakeup(remainingUs)
                              : wakeResult;

  // Recheck after arming the active-high wake source so a BUSY transition in
  // the setup window cannot leave the ESP asleep until the timeout.
  if (digitalRead(EPD_BUSY_PIN) == HIGH) {
    disableEpdLightSleepWake();
    return;
  }

  if (gpioResult != ESP_OK || wakeResult != ESP_OK || timerResult != ESP_OK) {
    deviceLog("EPD Light-sleep setup failed: gpio=%d wake=%d timer=%d\n",
              gpioResult, wakeResult, timerResult);
    disableEpdLightSleepWake();
    waitBusyPolling(started);
    return;
  }

  esp_err_t sleepResult = esp_light_sleep_start();
  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  disableEpdLightSleepWake();

  if (sleepResult != ESP_OK) {
    deviceLog("EPD Light-sleep failed: %d\n", sleepResult);
    waitBusyPolling(started);
    return;
  }

  if (digitalRead(EPD_BUSY_PIN) == HIGH) return;

  if (wakeCause == ESP_SLEEP_WAKEUP_TIMER &&
      millis() - started >= EPD_BUSY_TIMEOUT_MS) {
    deviceLog("EPD busy timeout\n");
    return;
  }

  // A GPIO wake while BUSY is still low, or any unrelated wake source, is not
  // trustworthy. Continue with the bounded reduced-clock polling fallback.
  deviceLog("EPD Light-sleep unexpected wake: %d\n", wakeCause);
  waitBusyPolling(started);
}

void PhotoPainterDisplay::sendCommand(uint8_t command) {
  digitalWrite(EPD_DC_PIN, LOW);
  digitalWrite(EPD_CS_PIN, LOW);
  SPI.transfer(command);
  digitalWrite(EPD_CS_PIN, HIGH);
}

void PhotoPainterDisplay::sendData(uint8_t data) {
  digitalWrite(EPD_DC_PIN, HIGH);
  digitalWrite(EPD_CS_PIN, LOW);
  SPI.transfer(data);
  digitalWrite(EPD_CS_PIN, HIGH);
}

void PhotoPainterDisplay::sendBuffer(const uint8_t* data, size_t len) {
  digitalWrite(EPD_DC_PIN, HIGH);
  digitalWrite(EPD_CS_PIN, LOW);
  const size_t chunk = 4096;
  for (size_t offset = 0; offset < len; offset += chunk) {
    SPI.writeBytes(data + offset, min(chunk, len - offset));
  }
  digitalWrite(EPD_CS_PIN, HIGH);
}

uint8_t PhotoPainterDisplay::mapGfxColor(uint16_t color) const {
  if (color == 0) return PP_BLACK;
  if (color == 1 || color == 0xFFFF) return PP_WHITE;
  return PP_BLACK;
}

static void drawCenteredLines(const String& title, const String& message) {
  if (!display.begin()) {
    deviceLog("Display allocation/init failed\n");
    return;
  }

  display.clear(PP_WHITE);
  display.setTextColor(PP_BLACK);
  display.setTextSize(3);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((DISPLAY_WIDTH - w) / 2, 64);
  display.print(title);

  display.setTextSize(2);
  int y = 170;
  int start = 0;
  while (start < static_cast<int>(message.length())) {
    int nl = message.indexOf('\n', start);
    if (nl < 0) nl = message.length();
    String line = message.substring(start, nl);
    display.getTextBounds(line, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((DISPLAY_WIDTH - w) / 2, y);
    display.print(line);
    y += 34;
    start = nl + 1;
  }

  display.setTextSize(1);
  String version = String("FW ") + FW_VERSION_STR;
  display.getTextBounds(version, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((DISPLAY_WIDTH - w) / 2, DISPLAY_HEIGHT - 28);
  display.print(version);
  display.refresh();
}

void initDisplay() {
  if (!display.begin()) {
    deviceLog("Display allocation/init failed\n");
  }
}

void showLoadingScreen() {
  drawCenteredLines("TRMNL", "Loading...");
}

void showSetupScreen(const String& message) {
  invalidateImageCache("setup_screen");
  drawCenteredLines("PhotoPainter TRMNL", message);
}

void showErrorScreen(const String& message) {
  invalidateImageCache("error_screen");
  drawCenteredLines("Error", message);
}
