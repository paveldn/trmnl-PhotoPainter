#pragma once

#include <Arduino.h>

static constexpr int DISPLAY_WIDTH = 800;
static constexpr int DISPLAY_HEIGHT = 480;

static constexpr int EPD_DC_PIN = 8;
static constexpr int EPD_CS_PIN = 9;
static constexpr int EPD_SCK_PIN = 10;
static constexpr int EPD_MOSI_PIN = 11;
static constexpr int EPD_RST_PIN = 12;
static constexpr int EPD_BUSY_PIN = 13;

static constexpr int I2C_SDA_PIN = 47;
static constexpr int I2C_SCL_PIN = 48;
static constexpr int AXP2101_IRQ_PIN = 21;

static constexpr int BUTTON_BOOT_PIN = 0;
static constexpr int LED_RED_PIN = 45;
static constexpr int LED_GREEN_PIN = 42;

