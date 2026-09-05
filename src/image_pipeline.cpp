#include "image_pipeline.h"

#include <HTTPClient.h>
#include <PNGdec.h>
#undef INTELSHORT
#undef INTELLONG
#undef MOTOSHORT
#undef MOTOLONG
#include <JPEGDEC.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "api_helpers.h"
#include "display.h"
#include "trmnl_keys.h"

static constexpr int MAX_IMAGE_SIZE = 1200000;
static bool imageFramebufferChanged = false;
static String imageDisplayError;
static PNG pngDecoder;
static JPEGDEC jpegDecoder;
static int imageXOffset = 0;
static int imageYOffset = 0;

extern Preferences prefs;
extern String apiBaseUrl;
extern String apiKey;

extern void deviceLog(const char* fmt, ...);
extern void disableWiFiPS();

String getWifiBand() {
  wifi_bandwidth_t bandwidth;
  if (esp_wifi_get_bandwidth(WIFI_IF_STA, &bandwidth) == ESP_OK) {
    switch (bandwidth) {
      case WIFI_BW_HT20: return "HT20";
      case WIFI_BW_HT40: return "HT40";
#ifdef WIFI_BW_HT80
      case WIFI_BW_HT80: return "HT80";
#endif
#ifdef WIFI_BW_HT160
      case WIFI_BW_HT160: return "HT160";
#endif
      default: break;
    }
  }
  return "";
}

static uint16_t read16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t read32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

static uint8_t nearestPanelColor(uint8_t r, uint8_t g, uint8_t b) {
  struct PaletteColor {
    uint8_t code;
    uint8_t r;
    uint8_t g;
    uint8_t b;
  };
  static constexpr PaletteColor palette[] = {
      {PP_BLACK, 0, 0, 0},
      {PP_WHITE, 255, 255, 255},
      {PP_YELLOW, 255, 255, 0},
      {PP_RED, 255, 0, 0},
      {PP_BLUE, 0, 0, 255},
      {PP_GREEN, 0, 255, 0},
  };

  uint32_t bestDistance = UINT32_MAX;
  uint8_t best = PP_WHITE;
  for (const auto& color : palette) {
    int dr = static_cast<int>(r) - color.r;
    int dg = static_cast<int>(g) - color.g;
    int db = static_cast<int>(b) - color.b;
    uint32_t distance = dr * dr + dg * dg + db * db;
    if (distance < bestDistance) {
      bestDistance = distance;
      best = color.code;
    }
  }
  return best;
}

static void drawRgb565Pixel(int x, int y, uint16_t pixel) {
  uint8_t r = static_cast<uint8_t>(((pixel >> 11) & 0x1F) * 255 / 31);
  uint8_t g = static_cast<uint8_t>(((pixel >> 5) & 0x3F) * 255 / 63);
  uint8_t b = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
  display.drawNativePixel(x, y, nearestPanelColor(r, g, b));
}

static int drawPngLine(PNGDRAW* draw) {
  uint16_t pixels[DISPLAY_WIDTH];
  if (draw->iWidth > DISPLAY_WIDTH) return 0;

  pngDecoder.getLineAsRGB565(draw, pixels, PNG_RGB565_LITTLE_ENDIAN, 0xFFFFFFFF);
  int dstY = draw->y + imageYOffset;
  if (dstY < 0 || dstY >= DISPLAY_HEIGHT) return 1;

  for (int x = 0; x < draw->iWidth; ++x) {
    int dstX = x + imageXOffset;
    if (dstX >= 0 && dstX < DISPLAY_WIDTH) {
      drawRgb565Pixel(dstX, dstY, pixels[x]);
    }
  }
  return 1;
}

static int drawJpegBlock(JPEGDRAW* draw) {
  for (int y = 0; y < draw->iHeight; ++y) {
    int dstY = draw->y + y;
    if (dstY < 0 || dstY >= DISPLAY_HEIGHT) continue;

    for (int x = 0; x < draw->iWidthUsed; ++x) {
      int dstX = draw->x + x;
      if (dstX >= 0 && dstX < DISPLAY_WIDTH) {
        drawRgb565Pixel(dstX, dstY, draw->pPixels[y * draw->iWidth + x]);
      }
    }
  }
  return 1;
}

static bool decodePngToDisplay(uint8_t* data, size_t len) {
  int result = pngDecoder.openRAM(data, static_cast<int>(len), drawPngLine);
  if (result != PNG_SUCCESS) {
    deviceLog("PNG: open failed %d\n", result);
    return false;
  }

  int width = pngDecoder.getWidth();
  int height = pngDecoder.getHeight();
  if (width <= 0 || height <= 0 || width > DISPLAY_WIDTH) {
    deviceLog("PNG: unsupported size %dx%d\n", width, height);
    pngDecoder.close();
    return false;
  }

  imageXOffset = (DISPLAY_WIDTH - width) / 2;
  imageYOffset = (DISPLAY_HEIGHT - height) / 2;
  display.clear(PP_WHITE);
  result = pngDecoder.decode(nullptr, 0);
  pngDecoder.close();
  if (result != PNG_SUCCESS) {
    deviceLog("PNG: decode failed %d\n", result);
    return false;
  }
  return true;
}

static bool decodeJpegToDisplay(uint8_t* data, size_t len) {
  if (!jpegDecoder.openRAM(data, static_cast<int>(len), drawJpegBlock)) {
    deviceLog("JPEG: open failed %d\n", jpegDecoder.getLastError());
    return false;
  }

  int width = jpegDecoder.getWidth();
  int height = jpegDecoder.getHeight();
  if (width <= 0 || height <= 0 || width > DISPLAY_WIDTH || height > DISPLAY_HEIGHT) {
    deviceLog("JPEG: unsupported size %dx%d\n", width, height);
    jpegDecoder.close();
    return false;
  }

  imageXOffset = (DISPLAY_WIDTH - width) / 2;
  imageYOffset = (DISPLAY_HEIGHT - height) / 2;
  display.clear(PP_WHITE);
  int result = jpegDecoder.decode(imageXOffset, imageYOffset, 0);
  if (!result) deviceLog("JPEG: decode failed %d\n", jpegDecoder.getLastError());
  jpegDecoder.close();
  return result == 1;
}

static bool decodeBmpToDisplay(const uint8_t* data, size_t len) {
  if (len < 54 || data[0] != 'B' || data[1] != 'M') {
    deviceLog("BMP: bad header\n");
    return false;
  }

  uint32_t pixelOffset = read32(data + 10);
  uint32_t dibSize = read32(data + 14);
  if (dibSize < 40 || len < 14 + dibSize) {
    deviceLog("BMP: unsupported DIB header\n");
    return false;
  }

  int32_t width = static_cast<int32_t>(read32(data + 18));
  int32_t signedHeight = static_cast<int32_t>(read32(data + 22));
  uint16_t planes = read16(data + 26);
  uint16_t bpp = read16(data + 28);
  uint32_t compression = read32(data + 30);

  if (planes != 1 || compression != 0) {
    deviceLog("BMP: compressed/invalid file\n");
    return false;
  }
  if (!(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 24 || bpp == 32)) {
    deviceLog("BMP: unsupported bpp %u\n", bpp);
    return false;
  }

  bool topDown = signedHeight < 0;
  int32_t height = topDown ? -signedHeight : signedHeight;
  if (width <= 0 || height <= 0) {
    deviceLog("BMP: invalid size %ldx%ld\n", static_cast<long>(width), static_cast<long>(height));
    return false;
  }

  uint32_t rowBytes = ((static_cast<uint32_t>(width) * bpp + 31) / 32) * 4;
  if (pixelOffset + static_cast<size_t>(rowBytes) * height > len) {
    deviceLog("BMP: truncated pixel data\n");
    return false;
  }

  const uint8_t* palette = nullptr;
  uint32_t colorsUsed = read32(data + 46);
  if (bpp <= 8) {
    uint32_t paletteEntries = colorsUsed ? colorsUsed : (1UL << bpp);
    if (14 + dibSize + paletteEntries * 4 > pixelOffset) {
      deviceLog("BMP: bad palette\n");
      return false;
    }
    palette = data + 14 + dibSize;
  }

  display.clear(PP_WHITE);
  int xOffset = (DISPLAY_WIDTH - width) / 2;
  int yOffset = (DISPLAY_HEIGHT - height) / 2;

  for (int32_t y = 0; y < height; ++y) {
    int32_t srcY = topDown ? y : (height - 1 - y);
    const uint8_t* row = data + pixelOffset + static_cast<size_t>(srcY) * rowBytes;
    int dstY = y + yOffset;
    if (dstY < 0 || dstY >= DISPLAY_HEIGHT) continue;

    for (int32_t x = 0; x < width; ++x) {
      uint8_t r = 255, g = 255, b = 255;
      if (bpp == 24 || bpp == 32) {
        const uint8_t* px = row + x * (bpp / 8);
        b = px[0];
        g = px[1];
        r = px[2];
      } else {
        uint8_t index = 0;
        if (bpp == 8) {
          index = row[x];
        } else if (bpp == 4) {
          uint8_t packed = row[x / 2];
          index = (x & 1) ? (packed & 0x0F) : (packed >> 4);
        } else {
          uint8_t packed = row[x / 8];
          index = (packed >> (7 - (x & 7))) & 0x01;
        }
        const uint8_t* entry = palette + index * 4;
        b = entry[0];
        g = entry[1];
        r = entry[2];
      }

      int dstX = x + xOffset;
      if (dstX >= 0 && dstX < DISPLAY_WIDTH) {
        display.drawNativePixel(dstX, dstY, nearestPanelColor(r, g, b));
      }
    }
  }

  return true;
}

bool displayImage(const char* imageUrl) {
  deviceLog("Downloading image\n");
  imageFramebufferChanged = false;
  imageDisplayError = "";

  if (downloadAndDisplayImage(imageUrl)) {
    if (imageFramebufferChanged) {
      display.refresh();
      deviceLog("Display done\n");
    } else {
      deviceLog("Image unchanged\n");
    }
    return true;
  } else {
    deviceLog("Image display failed\n");
    if (imageDisplayError.length() > 0) {
      showErrorScreen(imageDisplayError);
    }
    return false;
  }
}

bool downloadAndDisplayImage(const char* url) {
  disableWiFiPS();

  HTTPClient http;
  String sUrl = String(url);
  http.begin(sUrl);
  if (sUrl.startsWith(apiBaseUrl)) {
    addAuthHeaders(http, WiFi.macAddress(), apiKey);
  }

#ifndef FORCE_IMAGE_REFRESH_ON_WAKE
  prefs.begin(NVS_NAMESPACE, true);
  String storedEtag = prefs.getString(KEY_IMAGE_ETAG, "");
  String storedLast = prefs.getString(KEY_IMAGE_LASTMOD, "");
  prefs.end();
  if (storedEtag.length() > 0) http.addHeader("If-None-Match", storedEtag);
  if (storedLast.length() > 0) http.addHeader("If-Modified-Since", storedLast);
#else
  deviceLog("Image force-refresh enabled\n");
#endif

  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  int code = http.GET();
#ifndef FORCE_IMAGE_REFRESH_ON_WAKE
  if (code == HTTP_CODE_NOT_MODIFIED) {
    deviceLog("Image HTTP 304 Not Modified\n");
    http.end();
    return true;
  }
#endif
  if (code != HTTP_CODE_OK) {
    deviceLog("Image HTTP %d\n", code);
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len <= 0 || len > MAX_IMAGE_SIZE) {
    deviceLog("Image invalid size: %d\n", len);
    imageDisplayError = "Image download failed\nInvalid image size\nCheck server output";
    http.end();
    return false;
  }

  uint8_t* buffer = static_cast<uint8_t*>(ps_malloc(len));
  if (!buffer) buffer = static_cast<uint8_t*>(malloc(len));
  if (!buffer) {
    deviceLog("Image allocation failed\n");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t bytesRead = stream->readBytes(buffer, len);
  String respEtag = http.header("ETag");
  String respLast = http.header("Last-Modified");
  http.end();

  bool success = false;
  if (bytesRead >= 2 && buffer[0] == 'B' && buffer[1] == 'M') {
    success = decodeBmpToDisplay(buffer, bytesRead);
    if (success) {
      imageFramebufferChanged = true;
    } else {
      imageDisplayError = "BMP decode failed\nUse 800x480 BMP\nCheck server output";
    }
  } else if (bytesRead >= 4 && buffer[0] == 0x89 && buffer[1] == 0x50 &&
             buffer[2] == 0x4E && buffer[3] == 0x47) {
    success = decodePngToDisplay(buffer, bytesRead);
    if (success) {
      imageFramebufferChanged = true;
    } else {
      imageDisplayError = "PNG decode failed\nUse an 800x480 image\nCheck server output";
    }
  } else if (bytesRead >= 2 && buffer[0] == 0xFF && buffer[1] == 0xD8) {
    success = decodeJpegToDisplay(buffer, bytesRead);
    if (success) {
      imageFramebufferChanged = true;
    } else {
      imageDisplayError = "JPEG decode failed\nUse an 800x480 image\nCheck server output";
    }
  } else {
    uint8_t first = bytesRead > 0 ? buffer[0] : 0;
    uint8_t second = bytesRead > 1 ? buffer[1] : 0;
    deviceLog("Unsupported image format: %02X %02X\n", first, second);
    imageDisplayError = "Image format error\nUse PNG, JPEG, or BMP\nCheck server output";
  }

  if (success && (respEtag.length() > 0 || respLast.length() > 0)) {
    prefs.begin(NVS_NAMESPACE, false);
    if (respEtag.length() > 0) prefs.putString(KEY_IMAGE_ETAG, respEtag);
    if (respLast.length() > 0) prefs.putString(KEY_IMAGE_LASTMOD, respLast);
    prefs.end();
  }

  free(buffer);
  return success;
}
