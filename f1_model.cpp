// Race-state model for the OpenF1 API — see f1_model.h.
#include "f1_model.h"

#include <ArduinoJson.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>

// ---- time helpers -------------------------------------------------------

static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

int64_t f1ParseIso(const char *s) {
  if (!s) return 0;
  int Y, M, D, h, m, sec;
  if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  int64_t ms = ((daysFromCivil(Y, M, D) * 24 + h) * 60 + m) * 60 + sec;
  ms *= 1000;
  const char *p = s + 19;
  if (*p == '.') {
    int frac = 0, digits = 0;
    for (++p; *p >= '0' && *p <= '9'; ++p)
      if (digits < 3) { frac = frac * 10 + (*p - '0'); ++digits; }
    while (digits++ < 3) frac *= 10;
    ms += frac;
  }
  if (*p == '+' || *p == '-') {       // API is always +00:00, but be correct
    int oh = 0, om = 0;
    if (sscanf(p + 1, "%2d:%2d", &oh, &om) >= 1) {
      int64_t off = (int64_t)(oh * 60 + om) * 60000;
      ms += (*p == '+') ? -off : off;
    }
  }
  return ms;
}

void f1FmtIso(int64_t ms, char *out, size_t n) {
  int64_t s = ms / 1000;
  int milli = (int)(ms % 1000);
  int64_t days = s / 86400;
  int rem = (int)(s % 86400);
  // civil from days (Howard Hinnant)
  int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int y = (int)yoe + (int)era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) ++y;
  snprintf(out, n, "%04d-%02u-%02uT%02d:%02d:%02d.%03d", y, m, d, rem / 3600,
           (rem / 60) % 60, rem % 60, milli);
}

void f1FmtLap(float sec, char *out, size_t n) {
  if (!(sec > 0)) { if (n) out[0] = 0; return; }
  int ms = (int)lroundf(sec * 1000.0f);
  int m = ms / 60000;
  ms -= m * 60000;
  if (m > 0) snprintf(out, n, "%d:%02d.%03d", m, ms / 1000, ms % 1000);
  else snprintf(out, n, "%d.%03d", ms / 1000, ms % 1000);
}

// ---- small helpers ------------------------------------------------------

static void copyStr(char *dst, size_t n, const char *src) {
  if (!n) return;
  if (!src) src = "";
  // Fold the handful of non-ASCII letters F1 names use: the UI fonts are
  // ASCII-only and would draw them as boxes.
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)src; *p && o + 1 < n; ++p) {
    if (*p < 0x80) { dst[o++] = (char)*p; continue; }
    if (*p == 0xC3 && p[1]) {
      unsigned char c = *++p;
      char r = '?';
      bool up = c < 0xA0;
      unsigned char l = up ? c + 0x20 : c;
      if (l >= 0xA0 && l <= 0xA5) r = 'a';
      else if (l == 0xA7) r = 'c';
      else if (l >= 0xA8 && l <= 0xAB) r = 'e';
      else if (l >= 0xAC && l <= 0xAF) r = 'i';
      else if (l == 0xB1) r = 'n';
      else if ((l >= 0xB2 && l <= 0xB6) || l == 0xB8) r = 'o';
      else if (l >= 0xB9 && l <= 0xBC) r = 'u';
      dst[o++] = (up && r != '?') ? (char)(r - 32) : r;
      continue;
    }
    while ((p[1] & 0xC0) == 0x80) ++p;   // skip the rest of another sequence
    dst[o++] = '?';
  }
  dst[o] = 0;
}

static bool has(const char *hay, const char *needle) { return hay && strstr(hay, needle); }

static F1Kind kindOf(const char *type) {
  if (type && !strcmp(type, "Race")) return F1_RACE;
  if (type && !strcmp(type, "Qualifying")) return F1_QUALI;
  return F1_PRACTICE;
}

// Parse a body into doc with a field filter. 1 = array ready, 0 = empty /
// "No results found", -1 = bad JSON.
static int parseRows(JsonDocument &doc, const char *json, size_t n,
                     const char *const *fields, JsonArray &rows) {
  if (!json || !n) return 0;
  JsonDocument filter;
  for (const char *const *f = fields; *f; ++f) filter[0][*f] = true;
  DeserializationError err =
      deserializeJson(doc, json, n, DeserializationOption::Filter(filter));
  if (err) return -1;
  if (!doc.is<JsonArray>()) return 0;      // {"detail": "..."}
  rows = doc.as<JsonArray>();
  return 1;
}

static void whoFromRow(F1Who &w, JsonObject r) {
  w.num = (uint8_t)(r["driver_number"] | 0);
  copyStr(w.code, sizeof w.code, r["name_acronym"] | "???");
  copyStr(w.last, sizeof w.last, r["last_name"] | "");
  copyStr(w.team, sizeof w.team, r["team_name"] | "");
  const char *c = r["team_colour"] | "808080";
  w.colour = (uint32_t)strtoul(c, nullptr, 16);
  if (!w.colour) w.colour = 0x808080;
}

static uint32_t fnv(uint32_t h, const char *s) {
  for (; s && *s; ++s) { h ^= (uint8_t)*s; h *= 16777619u; }
  return h;
}

// True if (t, h) was already recorded; otherwise records it.
bool F1Model::Seen::had(int64_t tt, uint32_t hh) {
  for (int i = 0; i < 16; ++i)
    if (t[i] == tt && h[i] == hh) return true;
  t[at] = tt; h[at] = hh;
  at = (at + 1) % 16;
  return false;
}

// Rows of another session (a stream can carry the tail of the previous one).
#define FOREIGN(r) (live.sessionKey && ((r)["session_key"] | live.sessionKey) != live.sessionKey)

// ---- model --------------------------------------------------------------

F1Model::F1Model() {
  memset(&live, 0, sizeof live);
  memset(&info, 0, sizeof info);
  resetLive(0, F1_PRACTICE);
}

void F1Model::resetLive(int sessionKey, F1Kind kind) {
  uint32_t v = live.version;
  memset(&live, 0, sizeof live);
  live.sessionKey = sessionKey;
  live.kind = kind;
  live.airT = live.trackT = -100;
  live.humidity = live.pressure = live.windSpeed = -1;
  live.windDir = -1;
  live.version = v + 1;
  curPos = curInt = curRc = curPit = curOvt = curWx = curRadio = curLoc = 0;
  memset(&seenRc_, 0, sizeof seenRc_);
  memset(&seenOvt_, 0, sizeof seenOvt_);
  alertsArmed = false;
  liveNow = false;
  aHead_ = aCount_ = 0;
  nPend_ = 0;
}

F1Driver *F1Model::driver(uint8_t num) {
  for (int i = 0; i < live.nDrv; ++i)
    if (live.drv[i].who.num == num) return &live.drv[i];
  return nullptr;
}

const F1Who *F1Model::who(uint8_t num) const {
  for (int i = 0; i < info.nWho; ++i)
    if (info.who[i].num == num) return &info.who[i];
  return nullptr;
}

void F1Model::pushAlert(uint8_t kind, uint8_t a, uint8_t b, uint8_t pos) {
  if (!alertsArmed) return;
  if (aCount_ == F1_ALERT_QUEUE) {
    if (kind >= ALERT_OVERTAKE) return;          // never displace a flag
    aHead_ = (aHead_ + 1) % F1_ALERT_QUEUE;      // drop the oldest
    --aCount_;
  }
  alerts_[(aHead_ + aCount_) % F1_ALERT_QUEUE] = {kind, a, b, pos};
  ++aCount_;
}

bool F1Model::popAlert(F1Alert &out) {
  if (!aCount_) return false;
  out = alerts_[aHead_];
  aHead_ = (aHead_ + 1) % F1_ALERT_QUEUE;
  --aCount_;
  return true;
}

void F1Model::sortOrder() {
  int n = live.nDrv;
  for (int i = 0; i < n; ++i) live.order[i] = (uint8_t)i;
  for (int i = 1; i < n; ++i) {                  // insertion sort, n <= 24
    uint8_t k = live.order[i];
    int pk = live.drv[k].pos ? live.drv[k].pos : 99;
    int j = i - 1;
    while (j >= 0) {
      int pj = live.drv[live.order[j]].pos ? live.drv[live.order[j]].pos : 99;
      if (pj < pk || (pj == pk && live.drv[live.order[j]].who.num < live.drv[k].who.num)) break;
      live.order[j + 1] = live.order[j];
      --j;
    }
    live.order[j + 1] = k;
  }
}

uint8_t F1Model::phaseOf(int64_t ms) const {
  uint8_t p = 0;
  for (uint8_t i = 1; i <= 3; ++i)
    if (live.phaseStart[i] && ms >= live.phaseStart[i]) p = i;
  return p;
}

float F1Model::shownBest(const F1Driver &d, bool *dim) const {
  if (dim) *dim = false;
  if (live.kind != F1_QUALI) return d.best[0];
  uint8_t ph = live.qPhase;
  if (ph > 3) ph = 3;
  if (d.best[ph] > 0) return d.best[ph];
  for (int p = ph - 1; p >= 0; --p)
    if (d.best[p] > 0) { if (dim) *dim = true; return d.best[p]; }
  return 0;
}

int F1Model::ingestDrivers(const char *json, size_t n) {
  static const char *const F[] = {"driver_number", "name_acronym", "last_name",
                                  "team_name", "team_colour", "headshot_url", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  for (JsonObject r : rows) {
    uint8_t num = (uint8_t)(r["driver_number"] | 0);
    if (!num) continue;
    F1Driver *d = driver(num);
    if (!d) {
      if (live.nDrv >= F1_MAX_DRIVERS) continue;
      d = &live.drv[live.nDrv++];
      memset(d, 0, sizeof *d);
      d->gap = d->interval = -1;
      d->compound = '?';
    }
    whoFromRow(d->who, r);
    const char *head = r["headshot_url"] | "";
    d->headUrl[0] = 0;                           // a URL cut short is no use
    if (strlen(head) < sizeof d->headUrl) strcpy(d->headUrl, head);
    ++cnt;
  }
  sortOrder();
  ++live.version;
  return cnt;
}

int F1Model::ingestPosition(const char *json, size_t n) {
  static const char *const F[] = {"date", "session_key", "driver_number", "position", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  bool changed = false;
  for (JsonObject r : rows) {
    if (FOREIGN(r)) continue;
    int64_t t = f1ParseIso(r["date"] | "");
    F1Driver *d = driver((uint8_t)(r["driver_number"] | 0));
    uint8_t p = (uint8_t)(r["position"] | 0);
    if (!d || !p || !t || t < d->posMs) continue;
    if (!d->gridPos) d->gridPos = p;
    if (d->pos != p) changed = true;
    d->pos = p;
    d->posMs = t;
    if (t > curPos) curPos = t;
    ++cnt;
  }
  if (changed) { sortOrder(); ++live.version; }
  return cnt;
}

static void gapField(JsonVariant v, float &sec, uint8_t &laps) {
  if (v.is<const char *>()) {                    // "+1 LAP", "+2 LAPS"
    const char *s = v.as<const char *>();
    int l = 0;
    while (*s && (*s < '0' || *s > '9')) ++s;
    l = atoi(s);
    laps = (uint8_t)(l > 0 ? l : 1);
    sec = -1;
  } else if (v.is<float>()) {
    sec = v.as<float>();
    laps = 0;
  }                                              // null: keep the last value
}

int F1Model::ingestIntervals(const char *json, size_t n) {
  static const char *const F[] = {"date", "session_key", "driver_number", "interval",
                                  "gap_to_leader", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  int64_t newest = curInt;
  for (JsonObject r : rows) {
    if (FOREIGN(r)) continue;
    int64_t t = f1ParseIso(r["date"] | "");
    F1Driver *d = driver((uint8_t)(r["driver_number"] | 0));
    if (!d || !t || t <= d->intMs) continue;
    gapField(r["gap_to_leader"], d->gap, d->gapLaps);
    gapField(r["interval"], d->interval, d->intLaps);
    d->intMs = t;
    if (t > newest) newest = t;
    ++cnt;
  }
  curInt = newest;
  if (newest > live.newestIntMs) live.newestIntMs = newest;
  if (cnt) ++live.version;
  return cnt;
}

static void ageTyre(F1Driver *d) {
  int cur = d->lapNo ? d->lapNo : 1;
  int age = (int)d->stintAge0 + (cur - (int)d->stintLap) + 1;
  d->tyreAge = (uint8_t)(age < 0 ? 0 : age > 250 ? 250 : age);
}

static uint8_t segClass(int v) {
  switch (v) {
    case 2048: return SEG_YELLOW;
    case 2049: return SEG_GREEN;
    case 2051: return SEG_PURPLE;
    case 2064: return SEG_PIT;
    default:   return SEG_NONE;
  }
}

int F1Model::ingestLaps(const char *json, size_t n, int64_t nowMs) {
  static const char *const F[] = {"session_key", "driver_number", "lap_number", "date_start",
                                  "lap_duration", "duration_sector_1", "duration_sector_2",
                                  "duration_sector_3", "segments_sector_1", "segments_sector_2",
                                  "segments_sector_3", "st_speed", nullptr};
  static const char *const SEC[] = {"duration_sector_1", "duration_sector_2", "duration_sector_3"};
  static const char *const SEG[] = {"segments_sector_1", "segments_sector_2", "segments_sector_3"};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  uint16_t lastLapNo[F1_MAX_DRIVERS] = {0};
  for (JsonObject r : rows) {
    if (FOREIGN(r)) continue;
    F1Driver *d = driver((uint8_t)(r["driver_number"] | 0));
    if (!d) continue;
    uint16_t lapNo = (uint16_t)(r["lap_number"] | 0);
    int64_t t0 = f1ParseIso(r["date_start"] | "");
    // Lap 1 of a race has no date_start; every other row without one is skipped.
    if (t0 && t0 > nowMs) continue;              // replay / stream delay
    if (lapNo > d->lapNo) { d->lapNo = lapNo; if (d->stintLap) ageTyre(d); }
    if (d->lapNo > live.lap) live.lap = d->lapNo;
    float dur = r["lap_duration"] | 0.0f;
    if (dur > 0 && (!t0 || t0 + (int64_t)(dur * 1000) <= nowMs)) {
      int idx = (int)(d - live.drv);
      if (lapNo >= lastLapNo[idx]) { lastLapNo[idx] = lapNo; d->lastLap = dur; }
      uint8_t ph = live.kind == F1_QUALI ? phaseOf(t0) : 0;
      if (!(d->best[ph] > 0) || dur < d->best[ph]) d->best[ph] = dur;
      if (!(live.fastest > 0) || dur < live.fastest) {
        live.fastest = dur;
        live.fastestNum = d->who.num;
        if (live.kind == F1_RACE && live.lap > 2) pushAlert(ALERT_FASTEST, d->who.num);
      }
    }
    // Sector times fill in one by one while the lap is being driven.
    float sec[3];
    bool any = false;
    int64_t done = t0;
    for (int i = 0; i < 3; ++i) {
      sec[i] = r[SEC[i]] | 0.0f;
      done += (int64_t)(sec[i] * 1000);
      if (!(sec[i] > 0) || (t0 && done > nowMs)) sec[i] = 0;   // not driven yet (replay)
      if (sec[i] > 0) any = true;
    }
    if (any && lapNo >= d->secLap) {
      d->secLap = lapNo;
      int k = 0;
      for (int i = 0; i < 3; ++i) {
        d->sector[i] = sec[i];
        d->nSeg[i] = 0;
        JsonArray segs = r[SEG[i]];
        for (JsonVariant v : segs) {
          if (k >= F1_MAX_SEGS) break;
          d->seg[k++] = sec[i] > 0 || !t0 ? segClass(v | 0) : (uint8_t)SEG_NONE;
          ++d->nSeg[i];
        }
      }
    }
    for (int i = 0; i < 3; ++i) {
      if (!(sec[i] > 0)) continue;
      if (!(d->bestSector[i] > 0) || sec[i] < d->bestSector[i]) d->bestSector[i] = sec[i];
      if (!(live.bestSector[i] > 0) || sec[i] < live.bestSector[i]) live.bestSector[i] = sec[i];
    }
    int trap = r["st_speed"] | 0;
    if (trap > 0 && lapNo >= d->secLap) {
      d->trap = (uint16_t)trap;
      if (trap > d->bestTrap) d->bestTrap = (uint16_t)trap;
      if (trap > live.bestTrap) live.bestTrap = (uint16_t)trap;
    }
    ++cnt;
  }
  if (cnt) ++live.version;
  return cnt;
}

int F1Model::ingestStints(const char *json, size_t n) {
  static const char *const F[] = {"session_key", "driver_number", "stint_number", "lap_start",
                                  "compound", "tyre_age_at_start", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  uint16_t bestStart[F1_MAX_DRIVERS] = {0};
  for (JsonObject r : rows) {
    if (FOREIGN(r)) continue;
    F1Driver *d = driver((uint8_t)(r["driver_number"] | 0));
    if (!d) continue;
    uint16_t ls = (uint16_t)(r["lap_start"] | 1);
    uint16_t cur = d->lapNo ? d->lapNo : 1;
    int idx = (int)(d - live.drv);
    // Newest stint that has already begun. In replay / delay mode later
    // stints are in the response too; live, a stint is sent as the car
    // leaves the pits, a lap or so before its first lap row. Rows also come
    // one at a time (stream), so an older stint must not undo a newer one.
    if (ls > cur + (liveNow ? 2 : 0)) continue;
    const char *c = r["compound"] | "";
    char comp = (*c && strcmp(c, "UNKNOWN")) ? c[0] : '?';
    int a0 = r["tyre_age_at_start"] | 0;
    a0 = a0 < 0 ? 0 : a0 > 200 ? 200 : a0;
    int st = (int)(r["stint_number"] | 1) - 1;
    // The whole history, for the tyre screen. A row can come again with the
    // compound filled in, so it is written over, not added.
    if (st >= 0 && st < F1_MAX_STINTS) {
      F1Stint &s = d->stint[st];
      if (s.compound != comp || s.age0 != a0 || s.lapStart != ls || d->nStint <= st) {
        s.compound = comp;
        s.age0 = (uint8_t)a0;
        s.lapStart = ls;
        if (d->nStint <= st) d->nStint = (uint8_t)(st + 1);
        ++cnt;
      }
    }
    if (ls < bestStart[idx] || ls < d->stintLap) continue;
    bestStart[idx] = ls;
    d->stintLap = ls;
    d->compound = comp;
    d->stintAge0 = (uint8_t)a0;
    ageTyre(d);
    d->stops = (uint8_t)(st < 0 ? 0 : st);
    ++cnt;
  }
  if (cnt) ++live.version;
  return cnt;
}

static bool noteWorthy(const char *cat, const char *scope, const char *msg) {
  if (!msg || !*msg) return false;
  if (!strcmp(cat, "Flag") && strcmp(scope, "Track")) return false;   // sector / blue flags
  static const char *const skip[] = {"OVERTAKE ", "SLIPPERY", "PASS HOLDERS", "RISK OF RAIN",
                                     "MARSHALS", "RECOVERY VEHICLE", "LIGHTS OFF", nullptr};
  for (const char *const *s = skip; *s; ++s)
    if (strstr(msg, *s)) return false;
  return true;
}

int F1Model::ingestRaceControl(const char *json, size_t n) {
  static const char *const F[] = {"date", "session_key", "category", "flag", "scope", "sector",
                                  "qualifying_phase", "message", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  for (JsonObject r : rows) {
    if (FOREIGN(r)) continue;
    int64_t t = f1ParseIso(r["date"] | "");
    const char *cat = r["category"] | "";
    const char *flag = r["flag"] | "";
    const char *scope = r["scope"] | "";
    const char *msg = r["message"] | "";
    // Several messages can carry the same date ("SESSION STARTED" and "RACE
    // START"), so a date alone cannot tell a repeat from a new one.
    if (!t || t < curRc || seenRc_.had(t, fnv(fnv(fnv(2166136261u, cat), flag), msg))) continue;
    if (t > curRc) curRc = t;
    ++cnt;
    int ph = r["qualifying_phase"] | 0;
    if (ph >= 1 && ph <= 3) {
      if (!live.phaseStart[ph] || t < live.phaseStart[ph]) live.phaseStart[ph] = t;
      if (ph > live.qPhase) live.qPhase = (uint8_t)ph;
    }
    F1Track before = live.track;
    if (!strcmp(cat, "SafetyCar")) {
      bool virt = has(msg, "VIRTUAL");
      if (has(msg, "DEPLOYED")) {
        live.track = virt ? TRK_VSC : TRK_SC;
        live.scEnding = false;
        pushAlert(virt ? ALERT_VSC : ALERT_SC);
      } else if (has(msg, "IN THIS LAP") || has(msg, "ENDING")) {
        live.scEnding = true;
        pushAlert(virt ? ALERT_VSC_ENDING : ALERT_SC_ENDING);
      }
    } else if (!strcmp(cat, "Flag")) {
      if (!strcmp(scope, "Track")) {
        if (!strcmp(flag, "RED")) {
          live.track = TRK_RED;
          pushAlert(ALERT_RED);
        } else if (!strcmp(flag, "GREEN") || !strcmp(flag, "CLEAR")) {
          live.yellowMask = 0;
          live.scEnding = false;
          live.track = TRK_CLEAR;
          live.finished = false;                 // next qualifying segment
          if (before == TRK_SC || before == TRK_VSC || before == TRK_RED) pushAlert(ALERT_GREEN);
        } else if (!strcmp(flag, "CHEQUERED")) {
          live.track = TRK_FINISHED;
          live.finished = true;
          if (live.kind == F1_RACE) {
            uint8_t w = live.nDrv ? live.drv[live.order[0]].who.num : 0;
            pushAlert(ALERT_CHEQUERED, w);
          }
        } else if (has(flag, "YELLOW")) {
          if (live.track == TRK_CLEAR) live.track = TRK_YELLOW;
        }
      } else if (!strcmp(scope, "Sector")) {
        int sec = r["sector"] | 0;
        if (sec >= 1 && sec <= 32) {
          uint32_t bit = 1u << (sec - 1);
          if (has(flag, "YELLOW")) live.yellowMask |= bit;
          else if (!strcmp(flag, "CLEAR")) live.yellowMask &= ~bit;
        }
        if (live.track == TRK_CLEAR && live.yellowMask) live.track = TRK_YELLOW;
        else if (live.track == TRK_YELLOW && !live.yellowMask) live.track = TRK_CLEAR;
      }
    } else if (!strcmp(cat, "SessionStatus")) {
      if (has(msg, "STARTED")) {
        if (!live.started && live.kind == F1_RACE) pushAlert(ALERT_START);
        live.started = true;
        live.finished = false;
      } else if (has(msg, "FINISHED")) {
        live.finished = true;
      }
    }
    if (noteWorthy(cat, scope, msg)) copyStr(live.note, sizeof live.note, msg);
  }
  if (cnt) ++live.version;
  return cnt;
}

int F1Model::ingestPit(const char *json, size_t n) {
  static const char *const F[] = {"date", "session_key", "driver_number", "lap_number",
                                  "lane_duration", "stop_duration", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  for (JsonObject r : rows) {
    if (FOREIGN(r)) continue;
    int64_t t = f1ParseIso(r["date"] | "");
    F1Driver *d = driver((uint8_t)(r["driver_number"] | 0));
    // The same row comes again once its durations are known.
    if (!d || !t || t < d->pitMs) continue;
    if (t > d->pitMs) { d->pitMs = t; d->pitLane = d->pitStop = 0; }
    if (t > curPit) curPit = t;
    float lane = r["lane_duration"] | 0.0f, stop = r["stop_duration"] | 0.0f;
    if (lane > 0) d->pitLane = lane;
    if (stop > 0) d->pitStop = stop;
    d->pitLap = (uint16_t)(r["lap_number"] | 0);
    if (d->pitLane > 0 && d->pitToldMs != t) {
      d->pitToldMs = t;
      if (live.kind == F1_RACE && live.started) pushAlert(ALERT_PIT, d->who.num);
    }
    ++cnt;
  }
  if (cnt) ++live.version;
  return cnt;
}

int F1Model::ingestOvertakes(const char *json, size_t n) {
  static const char *const F[] = {"date", "session_key", "overtaking_driver_number",
                                  "overtaken_driver_number", "position", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  for (JsonObject r : rows) {
    if (FOREIGN(r)) continue;
    int64_t t = f1ParseIso(r["date"] | "");
    uint8_t a = (uint8_t)(r["overtaking_driver_number"] | 0), b = (uint8_t)(r["overtaken_driver_number"] | 0);
    if (!t || t < curOvt || seenOvt_.had(t, ((uint32_t)a << 8) | b)) continue;
    if (t > curOvt) curOvt = t;
    ++cnt;
    uint8_t pos = (uint8_t)(r["position"] | 0);
    if (!alertsArmed || !pos || pos > 10) continue;
    if (nPend_ < (int)(sizeof pend_ / sizeof pend_[0])) pend_[nPend_++] = {t, a, b, pos};
  }
  return cnt;
}

// An overtake row is only a position swap. Hold each one briefly so the pit
// and interval feeds can catch up, then show it only if it looks like a pass
// on track: green running, neither car just pitted, and the two cars are
// still close together.
void F1Model::flushOvertakes(int64_t nowMs) {
  const int64_t HOLD = 12000;
  int keep = 0;
  for (int i = 0; i < nPend_; ++i) {
    PendingOvt &p = pend_[i];
    if (nowMs - p.t < HOLD) { pend_[keep++] = p; continue; }
    if (nowMs - p.t > 60000) continue;           // stale (boot catch-up)
    if (!live.started || live.finished || live.lap < 2) continue;
    if (live.track == TRK_SC || live.track == TRK_VSC || live.track == TRK_RED) continue;
    F1Driver *a = driver(p.a), *b = driver(p.b);
    if (!a || !b) continue;
    bool pitted = false;
    for (F1Driver *d : {a, b})
      if (d->pitMs && d->pitMs > p.t - 60000 && d->pitMs < p.t + 20000) pitted = true;
    if (pitted) continue;
    if (a->pos >= b->pos) continue;              // already swapped back
    if (a->gap < 0 && a->pos != 1) continue;
    if (b->gap < 0 || b->gapLaps || a->gapLaps) continue;
    float ga = a->pos == 1 ? 0 : a->gap;
    if (b->gap - ga > 5.0f) continue;            // far apart: pit stop or retirement
    pushAlert(ALERT_OVERTAKE, p.a, p.b, a->pos);
  }
  nPend_ = keep;
}

int F1Model::ingestWeather(const char *json, size_t n) {
  static const char *const F[] = {"date", "session_key", "air_temperature", "track_temperature",
                                  "rainfall", "humidity", "pressure", "wind_speed",
                                  "wind_direction", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  for (JsonObject r : rows) {
    if (FOREIGN(r)) continue;
    int64_t t = f1ParseIso(r["date"] | "");
    if (t <= curWx) continue;
    curWx = t;
    live.airT = r["air_temperature"] | -100.0f;
    float tt = r["track_temperature"] | -100.0f;
    if (tt != 0.0f || live.trackT < -50) live.trackT = tt;   // the feed sends 0.0 for "no reading"
    live.rain = (r["rainfall"] | 0.0f) > 0;
    live.humidity = r["humidity"] | -1.0f;
    live.pressure = r["pressure"] | -1.0f;
    live.windSpeed = r["wind_speed"] | -1.0f;
    live.windDir = (int16_t)(r["wind_direction"] | -1);
    ++cnt;
  }
  if (cnt) ++live.version;
  return cnt;
}

// ---- live telemetry (stream) ---------------------------------------------

int F1Model::ingestLocation(const char *json, size_t n) {
  static const char *const F[] = {"date", "session_key", "driver_number", "x", "y", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  for (JsonObject r : rows) {
    if (FOREIGN(r)) continue;
    int64_t t = f1ParseIso(r["date"] | "");
    F1Driver *d = driver((uint8_t)(r["driver_number"] | 0));
    if (!d || !t || t <= d->locMs) continue;
    int32_t x = r["x"] | 0, y = r["y"] | 0;
    if (!x && !y) continue;                      // no fix
    d->x = x; d->y = y; d->locMs = t;
    if (t > curLoc) curLoc = t;
    ++cnt;
  }
  if (cnt) ++live.fastVersion;
  return cnt;
}

int F1Model::ingestCarData(const char *json, size_t n) {
  static const char *const F[] = {"date", "session_key", "driver_number", "speed", "rpm",
                                  "n_gear", "throttle", "brake", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  for (JsonObject r : rows) {
    if (FOREIGN(r)) continue;
    int64_t t = f1ParseIso(r["date"] | "");
    F1Driver *d = driver((uint8_t)(r["driver_number"] | 0));
    if (!d || !t || t <= d->carMs) continue;
    d->carMs = t;
    d->speed = (int16_t)(r["speed"] | 0);
    d->rpm = (uint16_t)(r["rpm"] | 0);
    d->gear = (int8_t)(r["n_gear"] | 0);
    int th = r["throttle"] | 0, br = r["brake"] | 0;
    d->throttle = (uint8_t)(th < 0 ? 0 : th > 100 ? 100 : th);
    d->brake = (uint8_t)(br < 0 ? 0 : br > 100 ? 100 : br);
    ++cnt;
  }
  if (cnt) ++live.fastVersion;
  return cnt;
}

int F1Model::ingestTeamRadio(const char *json, size_t n) {
  static const char *const F[] = {"date", "session_key", "driver_number", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  int64_t newest = curRadio;
  for (JsonObject r : rows) {
    if (FOREIGN(r)) continue;
    int64_t t = f1ParseIso(r["date"] | "");
    if (!t || t <= curRadio) continue;
    if (t > newest) newest = t;
    uint8_t num = (uint8_t)(r["driver_number"] | 0);
    if (driver(num)) pushAlert(ALERT_RADIO, num);
    ++cnt;
  }
  curRadio = newest;
  return cnt;
}

// ---- calendar, standings, results --------------------------------------

int F1Model::ingestSessions(const char *json, size_t n) {
  static const char *const F[] = {"session_key", "meeting_key", "circuit_key", "session_type",
                                  "session_name", "date_start", "date_end", "is_cancelled", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  for (JsonObject r : rows) {
    if (cnt >= F1_MAX_SESSIONS) break;
    F1SessionInfo &s = info.sessions[cnt];
    s.key = r["session_key"] | 0;
    s.meetingKey = r["meeting_key"] | 0;
    s.circuitKey = r["circuit_key"] | 0;
    s.kind = kindOf(r["session_type"] | "");
    copyStr(s.name, sizeof s.name, r["session_name"] | "");
    s.startMs = f1ParseIso(r["date_start"] | "");
    s.endMs = f1ParseIso(r["date_end"] | "");
    s.cancelled = r["is_cancelled"] | false;
    if (s.key && s.startMs) ++cnt;
  }
  for (int i = 1; i < cnt; ++i) {                // sort by start
    F1SessionInfo k = info.sessions[i];
    int j = i - 1;
    while (j >= 0 && info.sessions[j].startMs > k.startMs) { info.sessions[j + 1] = info.sessions[j]; --j; }
    info.sessions[j + 1] = k;
  }
  info.nSessions = cnt;
  ++info.version;
  return cnt;
}

int F1Model::ingestMeetings(const char *json, size_t n) {
  static const char *const F[] = {"meeting_key", "meeting_name", "circuit_short_name",
                                  "country_name", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  for (JsonObject r : rows) {
    if (cnt >= F1_MAX_MEETINGS) break;
    F1MeetingInfo &m = info.meetings[cnt];
    m.key = r["meeting_key"] | 0;
    copyStr(m.name, sizeof m.name, r["meeting_name"] | "");
    copyStr(m.circuit, sizeof m.circuit, r["circuit_short_name"] | "");
    copyStr(m.country, sizeof m.country, r["country_name"] | "");
    if (m.key) ++cnt;
  }
  info.nMeetings = cnt;
  ++info.version;
  return cnt;
}

int F1Model::ingestWho(const char *json, size_t n) {
  static const char *const F[] = {"driver_number", "name_acronym", "last_name",
                                  "team_name", "team_colour", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  for (JsonObject r : rows) {
    uint8_t num = (uint8_t)(r["driver_number"] | 0);
    if (!num) continue;
    F1Who *w = nullptr;
    for (int i = 0; i < info.nWho; ++i)
      if (info.who[i].num == num) w = &info.who[i];
    if (!w) {
      if (info.nWho >= (int)(sizeof info.who / sizeof info.who[0])) continue;
      w = &info.who[info.nWho++];
    }
    whoFromRow(*w, r);
    ++cnt;
  }
  ++info.version;
  return cnt;
}

static void sortStand(F1StandRow *a, int n) {
  for (int i = 1; i < n; ++i) {
    F1StandRow k = a[i];
    int pk = k.pos ? k.pos : 99;
    int j = i - 1;
    while (j >= 0 && (a[j].pos ? a[j].pos : 99) > pk) { a[j + 1] = a[j]; --j; }
    a[j + 1] = k;
  }
}

int F1Model::ingestDriverStandings(const char *json, size_t n, int sessionKey) {
  static const char *const F[] = {"driver_number", "position_start", "position_current",
                                  "points_start", "points_current", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  for (JsonObject r : rows) {
    if (cnt >= (int)(sizeof info.drvStand / sizeof info.drvStand[0])) break;
    F1StandRow &s = info.drvStand[cnt];
    memset(&s, 0, sizeof s);
    s.num = (uint8_t)(r["driver_number"] | 0);
    s.pos = (uint8_t)(r["position_current"] | 0);
    s.posStart = (uint8_t)(r["position_start"] | 0);
    s.pts = r["points_current"] | 0.0f;
    s.ptsStart = r["points_start"] | 0.0f;
    if (s.num) ++cnt;
  }
  sortStand(info.drvStand, cnt);
  info.nDrvStand = cnt;
  info.standKey = sessionKey;
  ++info.version;
  return cnt;
}

int F1Model::ingestTeamStandings(const char *json, size_t n) {
  static const char *const F[] = {"team_name", "position_start", "position_current",
                                  "points_start", "points_current", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  for (JsonObject r : rows) {
    if (cnt >= F1_MAX_TEAMS) break;
    F1StandRow &s = info.teamStand[cnt];
    memset(&s, 0, sizeof s);
    copyStr(s.team, sizeof s.team, r["team_name"] | "");
    s.pos = (uint8_t)(r["position_current"] | 0);
    s.posStart = (uint8_t)(r["position_start"] | 0);
    s.pts = r["points_current"] | 0.0f;
    s.ptsStart = r["points_start"] | 0.0f;
    if (s.team[0]) ++cnt;
  }
  sortStand(info.teamStand, cnt);
  info.nTeamStand = cnt;
  ++info.version;
  return cnt;
}

// duration / gap_to_leader are numbers in races and practice, and a
// three-element array [Q1, Q2, Q3] in qualifying.
static float lastNumber(JsonVariant v) {
  if (v.is<JsonArray>()) {
    float out = -1;
    for (JsonVariant e : v.as<JsonArray>())
      if (e.is<float>()) out = e.as<float>();
    return out;
  }
  return v.is<float>() ? v.as<float>() : -1;
}

int F1Model::ingestResult(const char *json, size_t n, int sessionKey) {
  static const char *const F[] = {"driver_number", "position", "dnf", "dns", "dsq",
                                  "duration", "gap_to_leader", "number_of_laps", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  int cnt = 0;
  for (JsonObject r : rows) {
    if (cnt >= F1_MAX_DRIVERS) break;
    F1ResultRow &x = info.result[cnt];
    memset(&x, 0, sizeof x);
    x.num = (uint8_t)(r["driver_number"] | 0);
    x.pos = (uint8_t)(r["position"] | 0);
    x.dnf = r["dnf"] | false;
    x.dns = r["dns"] | false;
    x.dsq = r["dsq"] | false;
    x.laps = (uint16_t)(r["number_of_laps"] | 0);
    x.time = lastNumber(r["duration"]);
    JsonVariant g = r["gap_to_leader"];
    x.gap = -1;
    if (g.is<const char *>()) {
      const char *s = g.as<const char *>();
      while (*s && (*s < '0' || *s > '9')) ++s;
      int l = atoi(s);
      x.gapLaps = (uint8_t)(l > 0 ? l : 1);
    } else {
      x.gap = lastNumber(g);
    }
    if (x.num) ++cnt;
  }
  for (int i = 1; i < cnt; ++i) {                // classified first, by position
    F1ResultRow k = info.result[i];
    int pk = k.pos ? k.pos : 99;
    int j = i - 1;
    while (j >= 0 && (info.result[j].pos ? info.result[j].pos : 99) > pk) {
      info.result[j + 1] = info.result[j];
      --j;
    }
    info.result[j + 1] = k;
  }
  info.nResult = cnt;
  info.resultKey = sessionKey;
  ++info.version;
  return cnt;
}

int F1Model::ingestGrid(const char *json, size_t n, int meetingKey) {
  static const char *const F[] = {"position", "driver_number", "lap_duration", "session_key", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  // A sprint weekend has two grids: the newest qualifying session's counts.
  int newest = 0;
  for (JsonObject r : rows) { int k = r["session_key"] | 0; if (k > newest) newest = k; }
  int cnt = 0;
  for (JsonObject r : rows) {
    if (cnt >= F1_MAX_DRIVERS) break;
    if ((r["session_key"] | 0) != newest) continue;
    F1GridRow &g = info.grid[cnt];
    g.num = (uint8_t)(r["driver_number"] | 0);
    g.pos = (uint8_t)(r["position"] | 0);
    g.lap = r["lap_duration"] | 0.0f;
    if (g.num && g.pos) ++cnt;
  }
  for (int i = 1; i < cnt; ++i) {                // by grid position
    F1GridRow k = info.grid[i];
    int j = i - 1;
    while (j >= 0 && info.grid[j].pos > k.pos) { info.grid[j + 1] = info.grid[j]; --j; }
    info.grid[j + 1] = k;
  }
  info.nGrid = cnt;
  info.gridMeeting = meetingKey;
  ++info.version;
  return cnt;
}

bool F1Model::pickLap(const char *json, size_t n, int *driver, int64_t *startMs, float *durS,
                      float *sector1, float *sector2) {
  static const char *const F[] = {"driver_number", "date_start", "lap_duration", "is_pit_out_lap",
                                  "duration_sector_1", "duration_sector_2", nullptr};
  JsonDocument doc; JsonArray rows;
  if (parseRows(doc, json, n, F, rows) <= 0) return false;
  float best = 0;
  for (JsonObject r : rows) {
    float dur = r["lap_duration"] | 0.0f;
    int64_t t0 = f1ParseIso(r["date_start"] | "");
    if (!(dur > 30) || !t0 || (r["is_pit_out_lap"] | false)) continue;
    if (best > 0 && dur >= best) continue;
    best = dur;
    *driver = r["driver_number"] | 0;
    *startMs = t0;
    *durS = dur;
    if (sector1) *sector1 = r["duration_sector_1"] | 0.0f;
    if (sector2) *sector2 = r["duration_sector_2"] | 0.0f;
  }
  return best > 0;
}

int F1Model::ingestTrack(const char *json, size_t n, int circuitKey, int64_t lapStartMs,
                         float sector1, float sector2) {
  static const char *const F[] = {"x", "y", "date", nullptr};
  JsonDocument doc; JsonArray rows;
  int ok = parseRows(doc, json, n, F, rows);
  if (ok <= 0) return ok;
  // First pass: count usable rows and find the extent. (0,0) = no GPS fix.
  int total = 0;
  long minX = 0, maxX = 0, minY = 0, maxY = 0;
  for (JsonObject r : rows) {
    long x = r["x"] | 0L, y = r["y"] | 0L;
    if (!x && !y) continue;
    if (!total) { minX = maxX = x; minY = maxY = y; }
    if (x < minX) minX = x;
    if (x > maxX) maxX = x;
    if (y < minY) minY = y;
    if (y > maxY) maxY = y;
    ++total;
  }
  long w = maxX - minX, h = maxY - minY;
  if (total < 40 || w <= 0 || h <= 0) return 0;      // not a lap
  long span = w > h ? w : h;
  int step = (total + F1_MAX_TRACK - 1) / F1_MAX_TRACK;
  if (step < 1) step = 1;
  int i = 0, cnt = 0;
  // Where the car was when each sector time was set.
  bool split = lapStartMs && sector1 > 0 && sector2 > 0;
  int64_t t2 = lapStartMs + (int64_t)(sector1 * 1000), t3 = t2 + (int64_t)(sector2 * 1000);
  int sec2 = 0, sec3 = 0;
  for (JsonObject r : rows) {
    long x = r["x"] | 0L, y = r["y"] | 0L;
    if (!x && !y) continue;
    if (i++ % step) continue;
    if (cnt >= F1_MAX_TRACK) break;
    info.trackX[cnt] = (int16_t)((x - minX) * 1000 / span);
    info.trackY[cnt] = (int16_t)((maxY - y) * 1000 / span);   // the feed's y points up
    if (split) {
      int64_t t = f1ParseIso(r["date"] | "");
      if (t && !sec2 && t >= t2) sec2 = cnt;
      if (t && !sec3 && t >= t3) sec3 = cnt;
    }
    ++cnt;
  }
  if (!(sec2 > 0 && sec3 > sec2 && sec3 < cnt - 1)) sec2 = sec3 = 0;
  info.trackSec2 = (int16_t)sec2;
  info.trackSec3 = (int16_t)sec3;
  info.nTrack = cnt;
  info.trackW = (int16_t)(w * 1000 / span);
  info.trackH = (int16_t)(h * 1000 / span);
  info.trackOX = (int32_t)minX;
  info.trackOY = (int32_t)maxY;
  info.trackSpan = (int32_t)span;
  info.trackCircuit = circuitKey;
  info.nCorner = info.nMsec = 0;             // they were placed on the old outline
  ++info.version;
  return cnt;
}

int F1Model::ingestCircuit(const char *json, size_t n, int circuitKey) {
  if (!json || !n || info.trackCircuit != circuitKey || info.nTrack < 10 || info.trackSpan <= 0) return 0;
  JsonDocument filter;
  filter["corners"][0]["number"] = true;
  filter["corners"][0]["angle"] = true;
  filter["corners"][0]["trackPosition"] = true;
  filter["marshalSectors"][0]["trackPosition"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json, n, DeserializationOption::Filter(filter))) return -1;
  info.nCorner = info.nMsec = 0;
  // The file uses the position feed's coordinates. Every point has to lie on
  // the outline; one that does not means another layout, and none of it is used.
  bool fits = true;
  auto place = [&](JsonObject p, int *X, int *Y) -> int {
    float x = p["x"] | 0.0f, y = p["y"] | 0.0f;
    *X = (int)((x - info.trackOX) * 1000.0f / info.trackSpan);
    *Y = (int)((info.trackOY - y) * 1000.0f / info.trackSpan);
    long best = 0;
    int at = 0;
    for (int i = 0; i < info.nTrack; ++i) {
      long dx = info.trackX[i] - *X, dy = info.trackY[i] - *Y, d = dx * dx + dy * dy;
      if (!i || d < best) { best = d; at = i; }
    }
    if (p.isNull() || best > 50L * 50L) fits = false;      // 5 % of the circuit's longer side
    return at;
  };
  int nc = 0, nm = 0, X, Y;
  for (JsonObject c : doc["corners"].as<JsonArray>()) {
    if (nc >= F1_MAX_CORNERS) break;
    place(c["trackPosition"], &X, &Y);
    float a = (c["angle"] | 0.0f) * 0.01745329f;           // which way to push the number off the track
    info.cornerNum[nc] = (uint8_t)(c["number"] | 0);
    info.cornerX[nc] = (int16_t)X;
    info.cornerY[nc] = (int16_t)Y;
    info.cornerDX[nc] = (int8_t)(cosf(a) * 100);
    info.cornerDY[nc] = (int8_t)(-sinf(a) * 100);          // the feed's y points up
    ++nc;
  }
  for (JsonObject s : doc["marshalSectors"].as<JsonArray>()) {
    if (nm >= F1_MAX_MSEC) break;
    info.msecAt[nm++] = (int16_t)place(s["trackPosition"], &X, &Y);
  }
  if (!fits) nc = nm = 0;
  info.nCorner = (uint8_t)nc;
  info.nMsec = (uint8_t)(nm >= 3 ? nm : 0);
  ++info.version;
  return nc;
}

const F1SessionInfo *F1Model::sessionByKey(int key) const {
  for (int i = 0; i < info.nSessions; ++i)
    if (info.sessions[i].key == key) return &info.sessions[i];
  return nullptr;
}

const F1MeetingInfo *F1Model::meetingByKey(int key) const {
  for (int i = 0; i < info.nMeetings; ++i)
    if (info.meetings[i].key == key) return &info.meetings[i];
  return nullptr;
}

const F1SessionInfo *F1Model::activeSession(int64_t nowMs, int64_t leadMs, int64_t tailMs) const {
  const F1SessionInfo *best = nullptr;
  for (int i = 0; i < info.nSessions; ++i) {
    const F1SessionInfo &s = info.sessions[i];
    if (s.cancelled) continue;
    if (nowMs >= s.startMs - leadMs && nowMs <= s.endMs + tailMs) best = &s;   // latest wins
  }
  return best;
}

const F1SessionInfo *F1Model::nextSession(int64_t nowMs) const {
  for (int i = 0; i < info.nSessions; ++i)
    if (!info.sessions[i].cancelled && info.sessions[i].startMs > nowMs) return &info.sessions[i];
  return nullptr;
}

const F1SessionInfo *F1Model::lastEnded(int64_t nowMs, bool raceOnly) const {
  const F1SessionInfo *best = nullptr;
  for (int i = 0; i < info.nSessions; ++i) {
    const F1SessionInfo &s = info.sessions[i];
    if (s.cancelled || s.endMs > nowMs) continue;
    if (raceOnly && s.kind != F1_RACE) continue;
    best = &s;
  }
  return best;
}
