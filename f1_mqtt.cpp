#include "f1_mqtt.h"

#include <Arduino.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <mqtt_client.h>

#include "config.h"
#include "settings.h"
#include "stamp.h"

static const char *const TOPICS[MT_COUNT] = {
    "v1/position", "v1/intervals", "v1/race_control", "v1/laps",     "v1/pit",       "v1/stints",
    "v1/weather",  "v1/overtakes", "v1/location",     "v1/car_data", "v1/team_radio"};

// Rows of one topic waiting to be handed over: "[{...},{...}" without the
// closing bracket. Two sets, so a drain swaps them and parses outside the lock.
struct Batch { char *buf; size_t len, cap; };
static Batch s_in[MT_COUNT], s_out[MT_COUNT];
static const size_t BATCH_MAX = 512 * 1024;       // then the topic's backlog is dropped

static MqttSink s_sink;
static SemaphoreHandle_t s_mx, s_drainMx;
static esp_mqtt_client_handle_t s_cli;
static volatile uint8_t s_state;                  // 0 off, 1 connecting, 2 up, 3 down
static volatile uint32_t s_since;                 // millis() of the last state change
static volatile uint32_t s_lost;                  // stamp of when it stopped being up (or was first asked for); 0 while up or off
static volatile uint32_t s_rows, s_oldest;        // s_oldest: millis() of the oldest undrained row, 0 = none
static volatile uint8_t s_subs;
static volatile uint8_t s_fails;                  // attempts in a row that did not get as far as "up"

// A message bigger than the client's buffer arrives in pieces.
static char *s_frag;
static size_t s_fragLen;
static const size_t FRAG_MAX = 64 * 1024;
static int s_fragTopic = -1;

// The times first: a reader on another task that sees the new state must not
// pair it with the old state's time (an hour of "down" in an instant).
// s_lost is not moved by the retries of an outage (connecting <-> dropped):
// "how long has it not been up" used to start again at every attempt, so with
// the broker unreachable the fast polls ran for only 20 s of every 70.
static void setState(uint8_t st) {
  uint32_t now = stampNow();
  if (st == 0 || st == 2) s_lost = 0;
  else if (!s_lost) s_lost = now;
  s_since = now;
  s_state = st;
}

static volatile bool s_hold;                      // rows are collected but not handed over
void mqttHold(bool on) { s_hold = on; }

static int topicOf(const char *t, int n) {
  for (int i = 0; i < MT_COUNT; ++i)
    if ((int)strlen(TOPICS[i]) == n && !memcmp(TOPICS[i], t, n)) return i;
  return -1;
}

static void append(int topic, const char *data, size_t n) {
  // One row is an object; be tolerant of an array of them.
  while (n && isspace((unsigned char)*data)) { ++data; --n; }
  while (n && isspace((unsigned char)data[n - 1])) --n;
  if (n >= 2 && data[0] == '[' && data[n - 1] == ']') { ++data; n -= 2; }
  if (n < 2 || data[0] != '{') return;
  xSemaphoreTake(s_mx, portMAX_DELAY);
  Batch &b = s_in[topic];
  if (b.len + n + 3 > BATCH_MAX) b.len = 0;       // nobody is draining: keep the newest
  if (b.len + n + 3 > b.cap) {
    size_t cap = b.cap ? b.cap : 8192;
    while (cap < b.len + n + 3) cap *= 2;
    char *p = (char *)heap_caps_realloc(b.buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) { xSemaphoreGive(s_mx); return; }
    b.buf = p;
    b.cap = cap;
  }
  char sep = b.len ? ',' : '[';
  b.buf[b.len++] = sep;
  memcpy(b.buf + b.len, data, n);
  b.len += n;
  ++s_rows;
  if (!s_oldest) s_oldest = stampNow();   // not "| 1": see stamp.h (the MQTT task then drained at once, half the time)
  xSemaphoreGive(s_mx);
}

void mqttDrain() {
  if (!s_sink || !s_oldest) return;
  xSemaphoreTake(s_drainMx, portMAX_DELAY);
  xSemaphoreTake(s_mx, portMAX_DELAY);
  for (int i = 0; i < MT_COUNT; ++i) { Batch t = s_in[i]; s_in[i] = s_out[i]; s_out[i] = t; s_in[i].len = 0; }
  s_oldest = 0;
  xSemaphoreGive(s_mx);
  for (int i = 0; i < MT_COUNT; ++i) {
    Batch &b = s_out[i];
    if (!b.len) continue;
    b.buf[b.len++] = ']';                        // append() always leaves room for these two
    b.buf[b.len] = 0;
    s_sink((MqttTopic)i, b.buf, b.len);
    b.len = 0;
  }
  xSemaphoreGive(s_drainMx);
}

static void onEvent(void *, esp_event_base_t, int32_t id, void *data) {
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      s_subs = 0;
      for (int i = 0; i < MT_COUNT; ++i) esp_mqtt_client_subscribe(s_cli, TOPICS[i], 0);
      Serial.println("[mqtt] connected, subscribing");
      break;
    case MQTT_EVENT_SUBSCRIBED:
      if (++s_subs == MT_COUNT) {
        setState(2);
        s_fails = 0;
        Serial.printf("[mqtt] live stream up (%d topics)\n", (int)MT_COUNT);
      }
      break;
    case MQTT_EVENT_DISCONNECTED:
      if (s_state == 2) Serial.println("[mqtt] disconnected");
      if (s_state != 3 && s_fails < 250) ++s_fails;
      setState(3);
      break;
    case MQTT_EVENT_ERROR:
      // Said once per outage; the retries after that are silent.
      if (s_fails == 0 || s_state == 2) {
        if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
          Serial.printf("[mqtt] broker refused the connection (code %d) - token or account? Retrying.\n",
                        (int)e->error_handle->connect_return_code);
        else if (e->error_handle)
          Serial.printf("[mqtt] cannot reach the broker (tls 0x%x, errno %d) - retrying\n",
                        (unsigned)e->error_handle->esp_tls_last_esp_err,
                        (int)e->error_handle->esp_transport_sock_errno);
      }
      if (s_state != 3 && s_fails < 250) ++s_fails;
      setState(3);
      break;
    case MQTT_EVENT_DATA: {
      if (e->current_data_offset == 0) {
        s_fragTopic = topicOf(e->topic, e->topic_len);
        s_fragLen = 0;
      }
      if (s_fragTopic < 0) break;
      if (e->data_len == e->total_data_len) { append(s_fragTopic, e->data, e->data_len); }
      else {
        if (!s_frag) s_frag = (char *)heap_caps_malloc(FRAG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_frag || e->current_data_offset + e->data_len > (int)FRAG_MAX) { s_fragTopic = -1; break; }
        memcpy(s_frag + e->current_data_offset, e->data, e->data_len);
        s_fragLen = e->current_data_offset + e->data_len;
        if ((int)s_fragLen >= e->total_data_len) append(s_fragTopic, s_frag, s_fragLen);
      }
      // The network task normally drains; do it here if it has been busy -
      // but not while it is reading a session's history (mqttHold).
      uint32_t old = s_oldest;
      if (!s_hold && old && millis() - old > 400) mqttDrain();
      break;
    }
    default: break;
  }
}

void mqttBegin(MqttSink sink) {
  s_sink = sink;
  s_mx = xSemaphoreCreateMutex();
  s_drainMx = xSemaphoreCreateMutex();
  // The event handler reports failures in one line of its own; the
  // library's error lines would also land in the middle of a screenshot.
  for (const char *tag : {"mqtt_client", "transport_base", "esp-tls", "transport_ws", "esp-tls-mbedtls"})
    esp_log_level_set(tag, ESP_LOG_NONE);
}

void mqttStop() {
  if (!s_cli) return;
  esp_mqtt_client_stop(s_cli);
  esp_mqtt_client_destroy(s_cli);
  s_cli = nullptr;
  s_fails = 0;
  xSemaphoreTake(s_mx, portMAX_DELAY);          // rows not handed over yet belong to the old connection
  for (int i = 0; i < MT_COUNT; ++i) s_in[i].len = 0;
  s_oldest = 0;
  xSemaphoreGive(s_mx);
  setState(0);
  Serial.println("[mqtt] stopped");
}

void mqttConnect(const char *token) {
  if (s_state == 2) return;
  if (s_state == 1 && millis() - s_since < 20000) return;      // still trying
  // Dropped: try again after 3 s, then 6, 12 ... up to a minute.
  if (s_state == 3 && millis() - s_since < (s_fails > 5 ? 60000u : 1500u << s_fails)) return;
  if (s_cli) {
    esp_mqtt_client_stop(s_cli);
    esp_mqtt_client_destroy(s_cli);
    s_cli = nullptr;
  }
  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = MQTT_URI;
  if (!strncmp(MQTT_URI, "mqtts", 5)) cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.credentials.username = settings().f1User[0] ? settings().f1User : "darkside";
  cfg.credentials.authentication.password = token;
  cfg.session.keepalive = 30;
  // No automatic reconnect: the token is only good for an hour, so every
  // new connection is made here with a fresh one.
  cfg.network.disable_auto_reconnect = true;
  cfg.network.timeout_ms = 10000;
  cfg.buffer.size = 8192;
  cfg.buffer.out_size = 1024;
  cfg.task.stack_size = 12 * 1024;
  cfg.task.priority = 4;                // same as the network task (see CLAUDE.md)
  s_cli = esp_mqtt_client_init(&cfg);
  if (!s_cli) { Serial.println("[mqtt] client init failed"); setState(3); return; }
  esp_mqtt_client_register_event(s_cli, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, onEvent, nullptr);
  setState(1);
  if (esp_mqtt_client_start(s_cli) != ESP_OK) { Serial.println("[mqtt] client start failed"); setState(3); }
  else if (!s_fails) Serial.printf("[mqtt] connecting to %s\n", MQTT_URI);
}

bool mqttUp() { return s_state == 2; }
bool mqttFailed() { return s_state == 3; }
uint32_t mqttDownMs() {
  uint8_t st = s_state;
  uint32_t lost = s_lost;
  if (st == 2) return 0;
  if (st == 0) return 0xFFFFFFFFu;
  return lost ? millis() - lost + 1 : 0;        // 0: it came up between the two reads
}
uint32_t mqttRowCount() { return s_rows; }

void mqttPublish(const char *topic, const char *payload) {
  if (s_cli && s_state == 2) esp_mqtt_client_publish(s_cli, topic, payload, 0, 0, 0);
}
