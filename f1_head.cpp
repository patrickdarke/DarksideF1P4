#include "f1_head.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "f1_api.h"
#include "f1_app.h"

// LVGL's bundled PNG decoder (C; its header is not C++-clean). Output is
// RGBA, allocated with lv_mem_alloc.
extern "C" unsigned lodepng_decode32(unsigned char **out, unsigned *w, unsigned *h,
                                     const unsigned char *in, size_t insize);

struct Head { uint8_t num; uint16_t w, h; uint8_t *px; uint32_t triedMs; };
static Head s_heads[F1_MAX_DRIVERS + 6];
static int s_n = 0;

static Head *find(uint8_t num) {
  for (int i = 0; i < s_n; ++i)
    if (s_heads[i].num == num) return &s_heads[i];
  return nullptr;
}

const uint8_t *headGet(uint8_t num, uint16_t *w, uint16_t *h) {
  Head *x = find(num);
  if (!x || !x->px) return nullptr;
  *w = x->w;
  *h = x->h;
  return x->px;
}

bool headWanted(uint8_t num) {
  if (!num) return false;
  Head *x = find(num);
  if (!x) return s_n < (int)(sizeof s_heads / sizeof s_heads[0]);
  return !x->px && millis() - x->triedMs > 10 * 60000;   // failed: again in ten minutes
}

void headLoad(uint8_t num, const char *url) {
  appLock(portMAX_DELAY);
  Head *x = find(num);
  if (!x && s_n < (int)(sizeof s_heads / sizeof s_heads[0])) {
    x = &s_heads[s_n++];
    *x = {num, 0, 0, nullptr, 0};
  }
  if (x) x->triedMs = millis() | 1;
  appUnlock();
  if (!x || !url || strncmp(url, "https://", 8)) return;

  int st = apiGetUrl(url);
  if (st != 200 || apiLen() < 64) { Serial.printf("[head] #%d: HTTP %d\n", num, st); return; }
  // The size is in the PNG's header (IHDR: width and height at bytes 16-23,
  // big-endian). Checked before decoding: a large image would be unpacked in
  // full - tens of MB - only to be refused below.
  const unsigned char *png = (const unsigned char *)apiBody();
  auto be32 = [](const unsigned char *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | (unsigned)p[3];
  };
  if (memcmp(png + 12, "IHDR", 4) || be32(png + 16) > 256 || be32(png + 20) > 256) {
    Serial.printf("[head] #%d: not a PNG of 256 x 256 or less\n", num);
    return;
  }
  unsigned char *rgba = nullptr;
  unsigned w = 0, h = 0;
  unsigned err = lodepng_decode32(&rgba, &w, &h, png, apiLen());
  if (err || !rgba || !w || !h || w > 256 || h > 256) {
    Serial.printf("[head] #%d: not a usable PNG (%u, %ux%u)\n", num, err, w, h);
    if (rgba) lv_mem_free(rgba);
    return;
  }
  uint8_t *px = (uint8_t *)heap_caps_malloc((size_t)w * h * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (px) {
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
      const unsigned char *s = rgba + i * 4;
      uint16_t c = (uint16_t)(((s[0] & 0xF8) << 8) | ((s[1] & 0xFC) << 3) | (s[2] >> 3));
      px[i * 3] = (uint8_t)c;
      px[i * 3 + 1] = (uint8_t)(c >> 8);
      px[i * 3 + 2] = s[3];
    }
  }
  lv_mem_free(rgba);
  if (!px) return;
  appLock(portMAX_DELAY);
  x->w = (uint16_t)w;
  x->h = (uint16_t)h;
  x->px = px;
  appUnlock();
  Serial.printf("[head] #%d: %ux%u\n", num, w, h);
}
