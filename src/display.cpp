#include "display.h"

#include <SPI.h>
#include <esp_heap_caps.h>

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
  begun_ = true;
  return true;
}

void PhotoPainterDisplay::clear(uint8_t color) {
  if (!buffer_) return;
  memset(buffer_, (color << 4) | (color & 0x0F), framebufferSize());
}

void PhotoPainterDisplay::refresh() {
  if (!begun_ && !begin()) return;
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
  waitBusy();

  sendCommand(0x02);
  sendData(0x00);
  waitBusy();
}

void PhotoPainterDisplay::sleep() {
  if (!begun_) return;
  sendCommand(0x02);
  sendData(0x00);
  waitBusy();
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
  size_t index = (static_cast<size_t>(y) * DISPLAY_WIDTH + x) / 2;
  if ((x & 1) == 0) {
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

void PhotoPainterDisplay::waitBusy() {
  const unsigned long started = millis();
  while (digitalRead(EPD_BUSY_PIN) == LOW) {
    if (millis() - started > 120000UL) {
      deviceLog("EPD busy timeout\n");
      return;
    }
    delay(10);
  }
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

