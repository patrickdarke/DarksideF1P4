// Darkside F1 P4 — a personal live-timing display for Formula 1 on the
// Elecrow CrowPanel Advanced 10.1" (ESP32-P4 + ESP32-C6, 1024x600 EK79007
// MIPI-DSI, GT911 touch). Data: the OpenF1 API (https://openf1.org).
//
// Board bring-up follows Elecrow's own Arduino example for this panel
// (Lesson09): LDO3/LDO4 on, then ESP32_Display_Panel with the custom board
// file in this folder, then the LVGL 8 port.
#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_ldo_regulator.h>
#include <esp32-hal-hosted.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

#include "board_config.h"
#include "config.h"
#include "esp_panel_board_custom_conf.h"
#include "esp_panel_drivers_conf.h"
#include <ESP_Panel_Library.h>
#include <lvgl.h>

#include "f1_app.h"
#include "lvgl_v8_port.h"
#include "ui.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

static void ldoOn(int chan, int mv) {
  esp_ldo_channel_handle_t h = nullptr;
  esp_ldo_channel_config_t cfg = {};
  cfg.chan_id = chan;
  cfg.voltage_mv = mv;
  esp_err_t err = esp_ldo_acquire_channel(&cfg, &h);
  Serial.printf("LDO%d %d mV: %s\n", chan, mv, esp_err_to_name(err));
}

static void *psramCalloc(size_t n, size_t size) {
  void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// Free DMA-capable internal RAM: what the C6 link and the AES engine live on.
static void memLine(const char *when) {
  Serial.printf("[mem] %-12s dma free %6u, low %6u, largest %6u\n", when,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nDarkside F1 P4");

  // INTERNAL RAM LAW. The esp-hosted SDIO link to the C6 and the AES engine
  // need DMA-capable internal RAM, and with the core's defaults (malloc()
  // prefers internal RAM below 4 KB, mbedTLS always internal) only ~35 KB of
  // it was left once TLS was up. A burst of inbound packets then used it up:
  // downloads above ~60 KB stalled and the C6 link stayed dead until reboot
  // (seen on hardware 2026-11-27). So: plain malloc() - LVGL, ArduinoJson,
  // lwIP packet buffers - and mbedTLS go to PSRAM, which has ~31 MB free.
  heap_caps_malloc_extmem_enable(INTERNAL_MALLOC_MAX);
  mbedtls_platform_set_calloc_free(psramCalloc, free);
  memLine("boot");

  // The MIPI D-PHY (LDO3, 2.5 V) and the touch/I2C pull-ups (LDO4, 3.3 V)
  // are fed from the P4's internal regulators. Nothing lights up without them.
  ldoOn(3, 2500);
  ldoOn(4, 3300);

  Board *board = new Board();
  if (!board->init()) Serial.println("panel init FAILED");
#if LVGL_PORT_AVOID_TEARING_MODE
  board->getLCD()->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#endif
  if (!board->begin()) Serial.println("panel begin FAILED");
  if (board->getBacklight()) board->getBacklight()->setBrightness(BACKLIGHT_PCT);

  memLine("panel");
  appInit();   // before the UI: its LVGL timer reads the model under this lock
  lvgl_port_init(board->getLCD(), board->getTouch());
  lvgl_port_lock(-1);
  uiBegin();
  lvgl_port_unlock();

  // Wi-Fi rides the onboard ESP32-C6 over SDIO; this board's wiring differs
  // from the core's defaults, so the pins must be set before anything else.
  WiFi.setPins(C6_SDIO_CLK, C6_SDIO_CMD, C6_SDIO_D0, C6_SDIO_D1, C6_SDIO_D2, C6_SDIO_D3, C6_RESET);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_HOST);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTzTime(LOCAL_TZ, NTP_SERVER);
  MDNS.begin(MDNS_HOST);
  memLine("ui + wifi");

  appBegin();
}

// Send "S\n" on serial to get a screenshot: "\nSHOT w h len sum\n" then len
// bytes of RLE RGB565 (u16 count, u16 pixel, little-endian) and "\nSHOTEND\n".
// tools/serial_hub.py turns it into a PNG. Built in RAM first and sent with
// one write so other tasks' log lines cannot land inside the binary.
static void sendScreenshot() {
  if (!lvgl_port_lock(1000)) { Serial.println("[shot] LVGL busy"); return; }
  lv_disp_t *d = lv_disp_get_default();
  const uint16_t *px = (const uint16_t *)d->driver->draw_buf->buf1;
  const int w = d->driver->hor_res, h = d->driver->ver_res;
  const size_t n = (size_t)w * h;
  uint16_t *out = (uint16_t *)heap_caps_malloc(n * 4, MALLOC_CAP_SPIRAM);
  size_t o = 0;
  if (out) {
    for (size_t i = 0; i < n;) {
      uint16_t c = px[i], k = 0;
      while (i < n && px[i] == c && k < 0xFFFF) { i++; k++; }
      out[o++] = k; out[o++] = c;
    }
  }
  lvgl_port_unlock();
  if (!out) { Serial.println("[shot] no memory"); return; }
  const size_t len = o * 2;
  uint32_t sum = 0;
  for (size_t i = 0; i < len; i++) sum += ((uint8_t *)out)[i];
  char hdr[64];
  int hl = snprintf(hdr, sizeof hdr, "\nSHOT %d %d %u %u\n", w, h, (unsigned)len, (unsigned)sum);
  // One buffer, one write: header + body + trailer.
  uint8_t *msg = (uint8_t *)heap_caps_malloc(hl + len + 9, MALLOC_CAP_SPIRAM);
  if (msg) {
    memcpy(msg, hdr, hl);
    memcpy(msg + hl, out, len);
    memcpy(msg + hl + len, "\nSHOTEND\n", 9);
    Serial.write(msg, hl + len + 9);
    Serial.flush();
    heap_caps_free(msg);
  } else {
    Serial.println("[shot] no memory");
  }
  heap_caps_free(out);
}

void loop() {
  // Serial commands, one per line: "S" = screenshot, "M" = test the stream login, "G<path?query>" = test GET,
  // "V<n>" / "D<car>" / "I<n>" / "A<n>" = show a screen or alert (see ui.h).
  static char line[300];
  static size_t ll = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c != '\n' && c != '\r') {
      if (c >= ' ' && c < 127 && ll + 1 < sizeof line) line[ll++] = c;   // drop line noise
      continue;
    }
    line[ll] = 0;
    if (ll == 1 && line[0] == 'S') sendScreenshot();
    else if (ll == 1 && line[0] == 'M') appDebugStream();
    else if (ll > 1 && line[0] == 'G') appDebugFetch(line + 1);
    else if (ll > 1 && strchr("VDIA", line[0])) uiDebug(line);
    ll = 0;
  }
  // Everything runs in the LVGL and network tasks. A heartbeat on serial
  // makes a hung board obvious.
  static uint32_t last = 0;
  if (millis() - last > 30000) {
    last = millis();
    static bool told = false;
    if (!told && WiFi.status() == WL_CONNECTED) {   // which esp-hosted firmware is the C6 running?
      told = true;
      uint32_t a, b, c, x, y, z;
      hostedGetHostVersion(&a, &b, &c);
      hostedGetSlaveVersion(&x, &y, &z);
      Serial.printf("[sys] esp-hosted host %u.%u.%u, C6 %u.%u.%u%s\n", (unsigned)a, (unsigned)b, (unsigned)c,
                    (unsigned)x, (unsigned)y, (unsigned)z, hostedHasUpdate() ? " (C6 update available)" : "");
    }
    Serial.printf("[sys] heap %u dma (low %u) / %u psram, wifi %s\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "down");
  }
  delay(200);
}
