#include "f1_api.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>

#include "config.h"

static const size_t BODY_MAX = 6 * 1024 * 1024;   // a full race of /intervals is ~3 MB
static char  *s_body = nullptr;
static size_t s_len = 0, s_cap = 0;
static bool   s_overflow = false;
static esp_http_client_handle_t s_cli = nullptr;
static String s_token;
static uint32_t s_tokenExpMs = 0;   // millis() deadline
static uint32_t s_authRetryMs = 0;

static bool grow(size_t need) {
  if (need <= s_cap) return true;
  if (need > BODY_MAX) return false;
  size_t cap = s_cap ? s_cap : 16384;
  while (cap < need) cap *= 2;
  char *p = (char *)heap_caps_realloc(s_body, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = (char *)realloc(s_body, cap);
  if (!p) return false;
  s_body = p;
  s_cap = cap;
  return true;
}

static esp_err_t onEvent(esp_http_client_event_t *e) {
  if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
    if (!grow(s_len + e->data_len + 1)) { s_overflow = true; return ESP_OK; }
    memcpy(s_body + s_len, e->data, e->data_len);
    s_len += e->data_len;
    s_body[s_len] = 0;
  }
  return ESP_OK;
}

static void dropClient() {
  if (s_cli) { esp_http_client_cleanup(s_cli); s_cli = nullptr; }
}

static bool ensureClient() {
  if (s_cli) return true;
  esp_http_client_config_t cfg = {};
  cfg.url = "https://" OPENF1_HOST "/v1/sessions";
  cfg.event_handler = onEvent;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 15000;
  cfg.keep_alive_enable = true;
  cfg.buffer_size = 4096;
  cfg.buffer_size_tx = 4096;      // the bearer token is a long header
  s_cli = esp_http_client_init(&cfg);
  return s_cli != nullptr;
}

// Sliding one-minute window over every request this device makes (token
// requests included), so the 60/minute limit cannot be reached whatever the
// callers do.
static uint32_t s_sent[API_MAX_PER_MIN];
static int s_sentAt = 0;
static bool s_sentFull = false;

static void rateWait() {
  if (s_sentFull) {
    uint32_t age = millis() - s_sent[s_sentAt];      // oldest of the last N
    if (age < 60500) vTaskDelay(pdMS_TO_TICKS(60500 - age));
  }
  s_sent[s_sentAt] = millis();
  if (++s_sentAt >= API_MAX_PER_MIN) { s_sentAt = 0; s_sentFull = true; }
}

static int perform() {
  rateWait();
  s_len = 0;
  s_overflow = false;
  if (s_body) s_body[0] = 0;
  esp_err_t err = esp_http_client_perform(s_cli);
  if (err != ESP_OK) {
    Serial.printf("[api] transport error: %s\n", esp_err_to_name(err));
    dropClient();                 // reconnect on the next call
    return -1;
  }
  if (s_overflow) return -2;
  return esp_http_client_get_status_code(s_cli);
}

static String formEncode(const char *s) {
  String o;
  for (; *s; ++s) {
    char c = *s;
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') o += c;
    else { char b[4]; snprintf(b, sizeof b, "%%%02X", (unsigned char)c); o += b; }
  }
  return o;
}

static bool fetchToken() {
  if (!apiHasCreds()) return false;
  if (s_authRetryMs && (int32_t)(millis() - s_authRetryMs) < 0) return false;
  if (!ensureClient()) return false;
  String form = "username=" + formEncode(OPENF1_USERNAME) + "&password=" + formEncode(OPENF1_PASSWORD);
  esp_http_client_set_url(s_cli, "https://" OPENF1_HOST "/token");
  esp_http_client_set_method(s_cli, HTTP_METHOD_POST);
  esp_http_client_delete_header(s_cli, "Authorization");
  esp_http_client_set_header(s_cli, "Content-Type", "application/x-www-form-urlencoded");
  esp_http_client_set_post_field(s_cli, form.c_str(), form.length());
  int st = perform();
  if (s_cli) {
    esp_http_client_set_method(s_cli, HTTP_METHOD_GET);
    esp_http_client_set_post_field(s_cli, nullptr, 0);
    esp_http_client_delete_header(s_cli, "Content-Type");
  }
  s_token = "";
  if (st == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, s_body, s_len)) {
      const char *tok = doc["access_token"] | "";
      long exp = 3600;
      if (doc["expires_in"].is<const char *>()) exp = atol(doc["expires_in"].as<const char *>());
      else if (doc["expires_in"].is<long>()) exp = doc["expires_in"].as<long>();
      if (exp < 120) exp = 120;
      if (*tok) {
        s_token = tok;
        s_tokenExpMs = millis() + (uint32_t)(exp - 60) * 1000u;
        s_authRetryMs = 0;
        Serial.printf("[api] OpenF1 token ok, valid %ld s\n", exp);
        return true;
      }
    }
  }
  Serial.printf("[api] OpenF1 login failed (HTTP %d) — check OPENF1_USERNAME/PASSWORD\n", st);
  s_authRetryMs = millis() + 60000;   // do not hammer the login endpoint
  if (!s_authRetryMs) s_authRetryMs = 1;
  return false;
}

void apiBegin() { grow(65536); }

int32_t apiTokenLeftMs() { return apiAuthed() ? (int32_t)(s_tokenExpMs - millis()) : 0; }

bool apiToken(char *out, size_t n) {
  if (!apiHasCreds()) return false;
  // A token that is about to run out is no use for a connection meant to last.
  if (!apiAuthed() || (int32_t)(millis() - (s_tokenExpMs - 240000u)) > 0) fetchToken();
  if (!apiAuthed()) return false;
  strlcpy(out, s_token.c_str(), n);
  return true;
}

// A separate client: other hosts must not disturb the keep-alive connection
// to the API (or see its bearer token).
int apiGetUrl(const char *url) {
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.event_handler = onEvent;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 15000;
  cfg.buffer_size = 4096;
  cfg.max_redirection_count = 3;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) return -1;
  s_len = 0;
  s_overflow = false;
  if (s_body) s_body[0] = 0;
  esp_err_t err = esp_http_client_perform(c);
  int st = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
  esp_http_client_cleanup(c);
  return s_overflow ? -2 : st;
}
bool apiHasCreds() { return OPENF1_USERNAME[0] != 0; }
bool apiAuthed() { return s_token.length() > 0; }
const char *apiBody() { return s_body ? s_body : ""; }
size_t apiLen() { return s_len; }

int apiGet(const char *pathQuery) {
  if (apiHasCreds() && (!apiAuthed() || (int32_t)(millis() - s_tokenExpMs) > 0)) fetchToken();
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!ensureClient()) return -1;
    // '>' and '<' are OpenF1's comparison operators; they must be escaped.
    char url[420];
    size_t o = (size_t)snprintf(url, sizeof url, "https://" OPENF1_HOST "/v1/");
    for (const char *p = pathQuery; *p && o + 4 < sizeof url; ++p) {
      if (*p == '>') { memcpy(url + o, "%3E", 3); o += 3; }
      else if (*p == '<') { memcpy(url + o, "%3C", 3); o += 3; }
      else if (*p == ' ') { memcpy(url + o, "%20", 3); o += 3; }
      else url[o++] = *p;
    }
    url[o] = 0;
    esp_http_client_set_url(s_cli, url);
    esp_http_client_set_method(s_cli, HTTP_METHOD_GET);
    if (apiAuthed()) {
      String h = "Bearer " + s_token;
      esp_http_client_set_header(s_cli, "Authorization", h.c_str());
    } else {
      esp_http_client_delete_header(s_cli, "Authorization");
    }
    int st = perform();
    if (st == 401 && apiHasCreds() && attempt == 0) {   // token expired early
      s_token = "";
      if (fetchToken()) continue;
    }
    if (st < 0 && attempt == 0) continue;               // stale keep-alive socket
    return st;
  }
  return -1;
}
