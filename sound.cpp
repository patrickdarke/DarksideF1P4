#include "sound.h"

#include <Arduino.h>
#include <atomic>
#include <driver/i2s_std.h>

#include "config.h"

#if __has_include("start_sound.h")
#include "start_sound.h"
#define HAVE_START_SOUND 1
#else
#define HAVE_START_SOUND 0
#endif

void soundBegin() {
  pinMode(SPK_AMP_EN, OUTPUT);
  digitalWrite(SPK_AMP_EN, HIGH);   // active low: off
}

#if HAVE_START_SOUND && SOUND_VOLUME > 0

namespace {

// Nothing of this exists between clips: the I2S channel, its DMA buffers
// (internal RAM, which the C6 link and TLS live on - see the INTERNAL RAM LAW)
// and the task are made for one clip and given back after it.
//
// The buffers hold a quarter of a second, so the task can run at the network
// task's priority and still not run dry while the screen is being redrawn.
// Not higher: see the MQTT TASK PRIORITY LAW in CLAUDE.md.
constexpr int RING_BUFS = 8, BUF_FRAMES = 512;    // 16 KB of DMA RAM, 256 ms at 16 kHz
constexpr int CHUNK = 256;                        // frames per write
constexpr UBaseType_t TASK_PRIO = 4;
constexpr uint32_t TASK_STACK = 6144;

std::atomic<bool> s_busy{false};

// `out` holds left and right for each frame.
bool put(i2s_chan_handle_t tx, const int16_t *out, int frames) {
  size_t done = 0;
  size_t bytes = (size_t)frames * 2 * sizeof(int16_t);
  return i2s_channel_write(tx, out, bytes, &done, pdMS_TO_TICKS(1000)) == ESP_OK && done == bytes;
}

// Returns what went wrong, or null.
const char *play(const int16_t *pcm, size_t n, uint32_t hz) {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  cc.dma_desc_num = RING_BUFS;
  cc.dma_frame_num = BUF_FRAMES;
  cc.auto_clear = true;             // late with the next buffer -> silence, not the last one again
  i2s_chan_handle_t tx = nullptr;
  if (i2s_new_channel(&cc, &tx, nullptr) != ESP_OK) return "no I2S channel";

  // The amplifier takes one channel; which one is its business, so both carry the clip.
  i2s_std_config_t sc = {};
  sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(hz);
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  sc.gpio_cfg.bclk = (gpio_num_t)SPK_I2S_BCLK;
  sc.gpio_cfg.ws = (gpio_num_t)SPK_I2S_WS;
  sc.gpio_cfg.dout = (gpio_num_t)SPK_I2S_DOUT;
  sc.gpio_cfg.din = I2S_GPIO_UNUSED;

  const char *err = nullptr;
  if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK) err = "I2S set-up refused";
  else if (i2s_channel_enable(tx) != ESP_OK) err = "I2S would not start";
  if (err) {
    i2s_del_channel(tx);
    return err;
  }

  // The clocks run and the line is silent before the amplifier comes up, and
  // again before it goes down: no thump either end.
  vTaskDelay(pdMS_TO_TICKS(10));
  digitalWrite(SPK_AMP_EN, LOW);
  vTaskDelay(pdMS_TO_TICKS(30));

  int16_t out[CHUNK * 2];
  const int32_t gain = (int32_t)SOUND_VOLUME * SOUND_VOLUME;   // of 10000: the ear hears level by ratio
  for (size_t at = 0; at < n && !err;) {
    int frames = (int)min((size_t)CHUNK, n - at);
    for (int i = 0; i < frames; ++i) {
      int16_t v = (int16_t)((int32_t)pcm[at + i] * gain / 10000);
      out[2 * i] = out[2 * i + 1] = v;
    }
    if (!put(tx, out, frames)) err = "I2S write failed";
    at += frames;
  }
  // A write returns when its samples are queued, not played. One whole ring
  // of silence behind the clip: when that is queued, the clip has gone out.
  memset(out, 0, sizeof out);
  for (int left = RING_BUFS * BUF_FRAMES; left > 0 && !err; left -= CHUNK)
    if (!put(tx, out, min(left, CHUNK))) err = "I2S write failed";

  digitalWrite(SPK_AMP_EN, HIGH);
  vTaskDelay(pdMS_TO_TICKS(20));
  i2s_channel_disable(tx);
  i2s_del_channel(tx);
  return err;
}

void task(void *) {
  uint32_t t0 = millis();
  const char *err = play(kStartSound, sizeof kStartSound / sizeof kStartSound[0], START_SOUND_HZ);
  if (err) Serial.printf("[snd] race start: %s\n", err);
  else Serial.printf("[snd] race start played, %u ms, stack low %u\n", (unsigned)(millis() - t0),
                     (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  s_busy = false;
  vTaskDelete(nullptr);
}

}  // namespace

void soundRaceStart() {
  if (s_busy.exchange(true)) return;
  if (xTaskCreate(task, "f1snd", TASK_STACK, nullptr, TASK_PRIO, nullptr) != pdPASS) {
    s_busy = false;
    Serial.println("[snd] no memory for the sound task");
  }
}

#else

void soundRaceStart() {}

#endif
