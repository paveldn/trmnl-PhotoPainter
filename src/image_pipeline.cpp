#include "image_pipeline.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "api_helpers.h"
#include "display.h"
#include "trmnl_keys.h"

static constexpr int MAX_IMAGE_SIZE = 1200000;

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

void displayImage(const char* imageUrl) {
  deviceLog("Downloading image\n");
  if (downloadAndDisplayImage(imageUrl)) {
    display.refresh();
    deviceLog("Display done\n");
  } else {
    deviceLog("Image display failed\n");
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
    return false;
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

  if (respEtag.length() > 0 || respLast.length() > 0) {
    prefs.begin(NVS_NAMESPACE, false);
    if (respEtag.length() > 0) prefs.putString(KEY_IMAGE_ETAG, respEtag);
    if (respLast.length() > 0) prefs.putString(KEY_IMAGE_LASTMOD, respLast);
    prefs.end();
  }

  bool success = false;
  if (bytesRead >= 2 && buffer[0] == 'B' && buffer[1] == 'M') {
    success = decodeBmpToDisplay(buffer, bytesRead);
  } else {
    deviceLog("Unsupported image format: %02X %02X\n", buffer[0], buffer[1]);
  }

  free(buffer);
  return success;
}

