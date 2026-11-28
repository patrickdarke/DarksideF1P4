#include "settings.h"

#include <Arduino.h>
#include <WiFi.h>
#include <nvs.h>
#include <time.h>

#include "config.h"

namespace {

const char *const NVS_SPACE = "f1cfg";

enum Field : uint8_t { F_SSID, F_WPASS, F_USER, F_UPASS, F_TZ, F_UNITS };

Settings s_now;           // in use since boot
Settings s_next;          // s_now plus what the console has sent; "K!" saves it
uint8_t s_sent = 0;       // bit per Field in s_next that differs from what is stored
bool s_stored = false;    // something came from NVS at boot

struct Text {
  const char *key;        // console and NVS name
  Field field;
  char *now, *next;
  size_t cap;
};
const Text TEXTS[] = {
    {"ssid", F_SSID, s_now.wifiSsid, s_next.wifiSsid, sizeof s_now.wifiSsid},
    {"wpass", F_WPASS, s_now.wifiPass, s_next.wifiPass, sizeof s_now.wifiPass},
    {"user", F_USER, s_now.f1User, s_next.f1User, sizeof s_now.f1User},
    {"upass", F_UPASS, s_now.f1Pass, s_next.f1Pass, sizeof s_now.f1Pass},
    {"tz", F_TZ, s_now.tz, s_next.tz, sizeof s_now.tz},
};

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c |= 0x20;
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// Hex -> a string of at most cap-1 bytes. Returns what is wrong with it, or null.
const char *unhex(const char *hex, char *out, size_t cap) {
  size_t n = strlen(hex);
  if (n % 2) return "not hex";
  if (n / 2 >= cap) return "too long";
  for (size_t i = 0; i < n / 2; ++i) {
    int hi = hexDigit(hex[2 * i]), lo = hexDigit(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return "not hex";
    uint8_t b = (uint8_t)(hi << 4 | lo);
    if (b < 0x20 || b == 0x7F) return "control character";
    out[i] = (char)b;
  }
  out[n / 2] = 0;
  return nullptr;
}

void report() {
  char ssid[2 * sizeof s_now.wifiSsid];
  size_t o = 0;
  for (const char *p = s_now.wifiSsid; *p; ++p) o += snprintf(ssid + o, sizeof ssid - o, "%02x", (uint8_t)*p);
  ssid[o] = 0;
  Serial.printf("[cfg] v1 fw=%s ssid=%s login=%d tz=%s units=%c%c stored=%d net=%d clock=%d\n", FW_VERSION, ssid,
                s_now.f1User[0] != 0, s_now.tz, s_now.fahrenheit ? 'F' : 'C', s_now.mph ? 'M' : 'K', s_stored,
                WiFi.status() == WL_CONNECTED, time(nullptr) > 1600000000);
}

void restart(const char *why) {
  Serial.printf("[cfg] %s - restarting\n", why);
  Serial.flush();
  delay(300);
  ESP.restart();
}

void save() {
  if (!s_next.wifiSsid[0]) {
    Serial.println("[cfg] bad: no Wi-Fi name");
    return;
  }
  nvs_handle_t h;
  if (nvs_open(NVS_SPACE, NVS_READWRITE, &h) != ESP_OK) {
    Serial.println("[cfg] bad: the panel's storage would not open");
    return;
  }
  bool ok = true;
  for (const Text &t : TEXTS)
    if (s_sent & (1u << t.field)) ok &= nvs_set_str(h, t.key, t.next) == ESP_OK;
  if (s_sent & (1u << F_UNITS)) ok &= nvs_set_u8(h, "units", (uint8_t)(s_next.fahrenheit | s_next.mph << 1)) == ESP_OK;
  ok &= nvs_commit(h) == ESP_OK;
  nvs_close(h);
  if (!ok) {
    Serial.println("[cfg] bad: the panel's storage would not take it");
    return;
  }
  restart("saved");
}

void forget() {
  nvs_handle_t h;
  if (nvs_open(NVS_SPACE, NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
  }
  restart("cleared");
}

}  // namespace

const Settings &settings() { return s_now; }

void settingsLoad() {
  strlcpy(s_now.wifiSsid, WIFI_SSID, sizeof s_now.wifiSsid);
  strlcpy(s_now.wifiPass, WIFI_PASS, sizeof s_now.wifiPass);
  strlcpy(s_now.f1User, OPENF1_USERNAME, sizeof s_now.f1User);
  strlcpy(s_now.f1Pass, OPENF1_PASSWORD, sizeof s_now.f1Pass);
  strlcpy(s_now.tz, LOCAL_TZ, sizeof s_now.tz);
  s_now.fahrenheit = TEMP_FAHRENHEIT;
  s_now.mph = SPEED_MPH;

  nvs_handle_t h;
  if (nvs_open(NVS_SPACE, NVS_READONLY, &h) == ESP_OK) {   // not there until the first "K!"
    for (const Text &t : TEXTS) {
      char v[sizeof s_now.f1User];
      size_t n = sizeof v;                                 // comes back with the terminator counted
      if (nvs_get_str(h, t.key, v, &n) == ESP_OK && n <= t.cap) {
        memcpy(t.now, v, n);
        s_stored = true;
      }
    }
    uint8_t u;
    if (nvs_get_u8(h, "units", &u) == ESP_OK) {
      s_now.fahrenheit = u & 1;
      s_now.mph = u & 2;
      s_stored = true;
    }
    nvs_close(h);
  }
  s_next = s_now;
  Serial.printf("[cfg] Wi-Fi %s, OpenF1 login %s, %s%s\n", s_now.wifiSsid[0] ? "set" : "NOT SET",
                s_now.f1User[0] ? "set" : "none", s_now.tz, s_stored ? " (stored on the panel)" : "");
}

void settingsCommand(const char *line) {
  const char *cmd = line + 1;
  if (!strcmp(cmd, "?")) return report();
  if (!strcmp(cmd, "!")) return save();
  if (!strcmp(cmd, "0")) return forget();

  const char *eq = strchr(cmd, '=');
  if (!eq) {
    Serial.println("[cfg] bad: not a command");
    return;
  }
  size_t kl = (size_t)(eq - cmd);
  const char *val = eq + 1;
  if (kl == 5 && !strncmp(cmd, "units", 5)) {
    if (!strchr("FC", val[0]) || !val[0] || !strchr("MK", val[1]) || !val[1] || val[2]) {
      Serial.println("[cfg] bad: units are F or C, then M or K");
      return;
    }
    s_next.fahrenheit = val[0] == 'F';
    s_next.mph = val[1] == 'M';
    s_sent |= 1u << F_UNITS;
    Serial.println("[cfg] ok units");
    return;
  }
  for (const Text &t : TEXTS) {
    if (strlen(t.key) != kl || strncmp(cmd, t.key, kl)) continue;
    char v[sizeof s_now.f1User];
    if (const char *why = unhex(val, v, t.cap)) {
      Serial.printf("[cfg] bad: %s %s\n", t.key, why);
      return;
    }
    strlcpy(t.next, v, t.cap);
    s_sent |= 1u << t.field;
    Serial.printf("[cfg] ok %s\n", t.key);
    return;
  }
  Serial.println("[cfg] bad: no such setting");
}
