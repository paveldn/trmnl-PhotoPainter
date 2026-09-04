#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>

#include "hardware.h"

enum PhotoPainterColor : uint8_t {
  PP_BLACK = 0,
  PP_WHITE = 1,
  PP_YELLOW = 2,
  PP_RED = 3,
  PP_BLUE = 5,
  PP_GREEN = 6,
};

class PhotoPainterDisplay : public Adafruit_GFX {
public:
  PhotoPainterDisplay();

  bool begin();
  void clear(uint8_t color = PP_WHITE);
  void refresh();
  void sleep();
  uint8_t* framebuffer();
  size_t framebufferSize() const;

  void drawPixel(int16_t x, int16_t y, uint16_t color) override;
  void drawNativePixel(int16_t x, int16_t y, uint8_t color);

private:
  void resetPanel();
  void waitBusy();
  void sendCommand(uint8_t command);
  void sendData(uint8_t data);
  void sendBuffer(const uint8_t* data, size_t len);
  uint8_t mapGfxColor(uint16_t color) const;

  uint8_t* buffer_;
  bool begun_;
};

extern PhotoPainterDisplay display;

void initDisplay();
void showLoadingScreen();
void showSetupScreen(const String& message);
void showErrorScreen(const String& message);

