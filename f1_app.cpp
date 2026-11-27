#include "f1_app.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <sys/time.h>
#include <initializer_list>

#include "config.h"
#include "f1_api.h"
#include "f1_head.h"
#include "f1_mqtt.h"

static F1Model *s_model;
static AppStatus s_status;
static SemaphoreHandle_t s_mutex;

// Static task stack: internal RAM by construction (this core will otherwise
// put a large heap-allocated stack in PSRAM, which is slow for TLS).
static StackType_t s_stack[20 * 1024 / sizeof(StackType_t)];
static StaticTask_t s_tcb;

bool appLock(uint32_t waitMs) { return s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(waitMs)) == pdTRUE; }
void appUnlock() { xSemaphoreGive(s_mutex); }
F1Model &appModel() { return *s_model; }
const AppStatus &appStatus() { return s_status; }

int64_t appWallMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1700000000) return 0;
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void setMsg(const char *fmt, ...) {
  char b[sizeof s_status.msg];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  appLock(portMAX_DELAY);
  strlcpy(s_status.msg, b, sizeof s_status.msg);
  appUnlock();
  Serial.printf("[app] %s\n", b);
}

// ---- live session state --------------------------------------------------

static bool          s_live = false;
static F1SessionInfo s_cur;              // session on screen
static uint32_t      s_replayT0 = 0;     // millis() when the replay began
static int           s_calYear = 0;
static uint32_t      s_calMs = 0, s_idleMs = 0, s_liveRetryMs = 0, s_backoffUntil = 0;
static uint32_t      s_finishedMs = 0;
static int           s_doneKey = 0;      // session already shown to its end
static bool          s_idleForce = true;
static uint32_t      s_gapMs = POLL_GAP_MS;

static volatile bool    s_wantLoc = false;
static volatile uint8_t s_wantCar = 0;
void appWantFast(bool positions, uint8_t telemetryDriver) { s_wantLoc = positions; s_wantCar = telemetryDriver; }

static bool isReplay() { return REPLAY_SESSION_KEY != 0; }
static bool bounded() { return isReplay() || STREAM_DELAY_S > 0; }

// The stream cannot be cut off at a point in time, so replay and stream
// delay stay on REST - except on the bench, where tools/mqtt_bench.py plays
// a recorded session in step with the replay clock.
static bool streamWanted() {
#if !USE_MQTT
  return false;
#elif defined(MQTT_BENCH_HOST)
  return true;
#else
  return apiHasCreds() && !bounded();
#endif
}
static bool streamOk() { return streamWanted() && mqttDownMs() < STREAM_GRACE_MS; }

static int64_t dataNow() {
  if (isReplay())
    return s_cur.startMs + (int64_t)REPLAY_OFFSET_S * 1000 +
           (int64_t)(millis() - s_replayT0) * REPLAY_SPEED;
  return appWallMs() - (int64_t)STREAM_DELAY_S * 1000;
}

// GET + ingest under the lock. Returns the HTTP status; *rows gets the count.
template <typename Fn>
static int fetch(const char *q, Fn ingest, int *rows = nullptr) {
  static uint32_t last = 0;
  uint32_t since = millis() - last;
  if (since < s_gapMs) vTaskDelay(pdMS_TO_TICKS(s_gapMs - since));
  int st = apiGet(q);
  last = millis();
  int n = 0;
  if (st == 200) {
    appLock(portMAX_DELAY);
    n = ingest(apiBody(), apiLen());
    appUnlock();
    if (n < 0) Serial.printf("[app] bad JSON from %s\n", q);
  } else if (st == 429) {
    // Over the limit anyway (another client on the same account?): stay
    // silent for half a minute so the server's window can drain.
    s_backoffUntil = millis() + 30000;
    Serial.println("[app] HTTP 429 - pausing requests for 30 s");
  } else if (st != 404) {
    Serial.printf("[app] HTTP %d for %s\n", st, q);
  }
  if (rows) *rows = n;
  return st;
}

static void isoOf(int64_t ms, char *out, size_t n) { f1FmtIso(ms, out, n); }

// ---- stream ----------------------------------------------------------------

// One batch of pushed rows. Runs on the MQTT task or on the network task.
static void streamSink(MqttTopic t, const char *json, size_t len) {
  // Live, a lap row is sent the moment the lap starts: allow for the clocks
  // not agreeing to the millisecond.
  int64_t dn = dataNow() + (bounded() ? 0 : 3000);
  appLock(portMAX_DELAY);
  F1Model &m = *s_model;
  switch (t) {
    case MT_POSITION:     m.ingestPosition(json, len); break;
    case MT_INTERVALS:    m.ingestIntervals(json, len); break;
    case MT_RACE_CONTROL: m.ingestRaceControl(json, len); break;
    case MT_LAPS:         m.ingestLaps(json, len, dn); break;
    case MT_PIT:          m.ingestPit(json, len); break;
    case MT_STINTS:       m.ingestStints(json, len); break;
    case MT_WEATHER:      m.ingestWeather(json, len); break;
    case MT_OVERTAKES:    m.ingestOvertakes(json, len); break;
    case MT_LOCATION:     m.ingestLocation(json, len); break;
    case MT_CAR_DATA:     m.ingestCarData(json, len); break;
    case MT_TEAM_RADIO:   m.ingestTeamRadio(json, len); break;
    default: break;
  }
  appUnlock();
}

static char s_tok[2048];
static uint32_t s_streamTokUntil = 0;       // millis() when the stream's login token runs out
static uint32_t s_streamRows = 0, s_streamRowsMs = 0;

// Keep the stream connected while a session is on. Each (re)connection logs
// in with a current token: they only last an hour.
static void streamKeep() {
  if (!streamWanted() || mqttUp()) return;
  static uint32_t last = 0;
  if (last && millis() - last < 2000) return;
  last = millis() | 1;
#ifdef MQTT_BENCH_HOST
  strcpy(s_tok, "bench");
  s_streamTokUntil = millis() + 0x3FFFFFFF;
#else
  if (!apiToken(s_tok, sizeof s_tok)) return;
  s_streamTokUntil = millis() + (uint32_t)apiTokenLeftMs();
#endif
  s_streamRowsMs = millis();
  mqttConnect(s_tok);
}

// A connection that is up can still be no good: its token is about to run
// out (they last an hour; log in again with a new one before the broker
// drops us), or nothing has arrived for minutes (weather alone is a row a
// minute). Either way start afresh; REST covers the few seconds' gap.
static void streamCheck() {
  if (!mqttUp()) return;
  uint32_t rows = mqttRowCount();
  if (rows != s_streamRows) { s_streamRows = rows; s_streamRowsMs = millis(); }
  bool old = (int32_t)(s_streamTokUntil - millis()) < 120000;
  bool quiet = millis() - s_streamRowsMs > STREAM_QUIET_MS;
  if (!old && !quiet) return;
  Serial.printf("[mqtt] %s - connecting afresh\n", old ? "login token about to expire" : "nothing received for a while");
  mqttStop();
}

// ---- calendar ------------------------------------------------------------

static bool loadCalendar(int year) {
  char q[64];
  int n = 0;
  snprintf(q, sizeof q, "sessions?year=%d", year);
  int st = fetch(q, [](const char *b, size_t l) { return s_model->ingestSessions(b, l); }, &n);
  if (st != 200 || n <= 0) return false;
  snprintf(q, sizeof q, "meetings?year=%d", year);
  fetch(q, [](const char *b, size_t l) { return s_model->ingestMeetings(b, l); });
  s_calYear = year;
  s_calMs = millis();
  Serial.printf("[app] calendar %d: %d sessions\n", year, n);
  return true;
}

// Replay needs the session's own year before the calendar can be loaded.
static int replayYear() {
  char q[48];
  snprintf(q, sizeof q, "sessions?session_key=%d", (int)REPLAY_SESSION_KEY);
  if (apiGet(q) != 200) return 0;
  JsonDocument doc;
  if (deserializeJson(doc, apiBody(), apiLen()) || !doc.is<JsonArray>()) return 0;
  return doc[0]["year"] | 0;
}

// ---- live ----------------------------------------------------------------

static void traceTrack(int circuitKey, int64_t wall);

static void stopLive(bool done = false) {
  if (done) s_doneKey = s_cur.key;
  mqttStop();
  s_live = false;
  s_idleForce = true;
  appLock(portMAX_DELAY);
  s_status.mode = APP_IDLE;
  s_status.liveKey = 0;
  s_status.stream = false;
  appUnlock();
}

// Load everything a session has so far. False = nothing to show yet.
static bool startLive(const F1SessionInfo &s) {
  s_cur = s;
  if (isReplay()) s_replayT0 = millis();
  int64_t dn = dataNow();
  char iso[32], q[160];
  isoOf(dn, iso, sizeof iso);
  int K = s.key, n = 0;

  appLock(portMAX_DELAY);
  s_model->resetLive(K, s.kind);
  s_model->liveNow = !bounded();
  appUnlock();

  snprintf(q, sizeof q, "drivers?session_key=%d", K);
  int st = fetch(q, [](const char *b, size_t l) { return s_model->ingestDrivers(b, l); }, &n);
  if (st == 401 || st == 403) {
    appLock(portMAX_DELAY); s_status.authNeeded = true; appUnlock();
    return false;
  }
  if (st != 200 || n <= 0) return false;      // timing data not published yet

  // Start the stream first: rows pushed while the history below is being
  // read are applied as they come, and every ingest is safe to repeat.
  streamKeep();

  if (bounded()) snprintf(q, sizeof q, "race_control?session_key=%d&date<=%s", K, iso);
  else snprintf(q, sizeof q, "race_control?session_key=%d", K);
  fetch(q, [](const char *b, size_t l) { return s_model->ingestRaceControl(b, l); });

  if (bounded()) snprintf(q, sizeof q, "position?session_key=%d&date<=%s", K, iso);
  else snprintf(q, sizeof q, "position?session_key=%d", K);
  fetch(q, [](const char *b, size_t l) { return s_model->ingestPosition(b, l); });

  // All laps so far. (A date filter would drop race lap 1, which has no date.)
  snprintf(q, sizeof q, "laps?session_key=%d", K);
  fetch(q, [dn](const char *b, size_t l) { return s_model->ingestLaps(b, l, dn); });

  snprintf(q, sizeof q, "stints?session_key=%d", K);
  fetch(q, [](const char *b, size_t l) { return s_model->ingestStints(b, l); });

  if (s.kind == F1_RACE) {                    // every stop so far (a few dozen rows)
    if (bounded()) snprintf(q, sizeof q, "pit?session_key=%d&date<=%s", K, iso);
    else snprintf(q, sizeof q, "pit?session_key=%d", K);
    fetch(q, [](const char *b, size_t l) { return s_model->ingestPit(b, l); });
  }

  {                                           // the newest weather row
    char w0[32];
    isoOf(dn - 180000, w0, sizeof w0);
    if (bounded()) snprintf(q, sizeof q, "weather?session_key=%d&date>%s&date<=%s", K, w0, iso);
    else snprintf(q, sizeof q, "weather?session_key=%d&date>%s", K, w0);
    fetch(q, [](const char *b, size_t l) { return s_model->ingestWeather(b, l); });
  }

  // The car dots need this circuit's outline (normally traced while idle).
  appLock(portMAX_DELAY);
  int haveCircuit = s_model->info.trackCircuit;
  appUnlock();
  if (s.circuitKey && haveCircuit != s.circuitKey) traceTrack(s.circuitKey, appWallMs());

  appLock(portMAX_DELAY);
  F1Model &m = *s_model;                       // the stream may already have moved these on
  if (m.curInt < dn - 30000) m.curInt = dn - 30000;
  if (m.curOvt < dn) m.curOvt = dn;
  if (m.curWx < dn - 180000) m.curWx = dn - 180000;
  if (m.curRadio < dn) m.curRadio = dn;
  s_model->alertsArmed = true;
  s_status.mode = APP_LIVE;
  s_status.liveKey = K;
  s_status.authNeeded = false;
  s_status.dataNowMs = dn;
  appUnlock();
  s_live = true;
  s_finishedMs = 0;
  Serial.printf("[app] live: session %d (%s), %d drivers\n", K, s.name, n);
  return true;
}

// period: polling on its own; sync: re-read interval while the stream is
// delivering the same feed (0 = not polled at all then).
struct Feed { const char *name; uint32_t period, sync; bool raceOnly; uint32_t last; };
static Feed s_feeds[] = {
    {"race_control", POLL_FAST_MS, SYNC_FAST_MS, false, 0}, {"position", POLL_FAST_MS, SYNC_FAST_MS, false, 0},
    {"intervals", POLL_FAST_MS, SYNC_SLOW_MS, true, 0},     {"laps", POLL_LAPS_MS, SYNC_FAST_MS, false, 0},
    {"overtakes", POLL_LAPS_MS, SYNC_PIT_MS, true, 0},      {"pit", POLL_PIT_MS, SYNC_PIT_MS, true, 0},
    {"stints", POLL_SLOW_MS, SYNC_SLOW_MS, false, 0},       {"weather", POLL_WX_MS, SYNC_WX_MS, false, 0},
    {"location", POLL_LOC_MS, 0, false, 0},                 {"car_data", POLL_CAR_MS, 0, false, 0},
    {"team_radio", POLL_SLOW_MS, 0, false, 0},
};

// Runs on every pass of the live loop, whether or not a poll is due.
static bool liveTick(int64_t dn) {
  streamCheck();
  streamKeep();
  mqttDrain();
#ifdef MQTT_BENCH_HOST
  static uint32_t told = 0;                   // lets the bench broker follow the replay clock
  if (mqttUp() && millis() - told > 2000) {
    told = millis();
    char b[24];
    snprintf(b, sizeof b, "%lld", (long long)dn);
    mqttPublish("bench/clock", b);
  }
#endif
  appLock(portMAX_DELAY);
  s_model->flushOvertakes(dn);
  s_status.dataNowMs = dn;
  s_status.stream = mqttUp();
  bool finished = s_model->live.finished && s_model->live.kind == F1_RACE;
  appUnlock();
  return finished;
}

static void pollOne() {
  uint32_t now = millis();
  int64_t dn = dataNow();
  bool finished = liveTick(dn);
  if ((int32_t)(now - s_backoffUntil) < 0) { vTaskDelay(pdMS_TO_TICKS(250)); return; }
  bool stream = streamOk();
  Feed *due = nullptr;
  uint32_t worst = 0;
  for (Feed &f : s_feeds) {
    if (f.raceOnly && s_cur.kind != F1_RACE) continue;
    if (!OVERTAKE_TOASTS && !strcmp(f.name, "overtakes")) continue;
    if (!strcmp(f.name, "location") && !s_wantLoc) continue;
    if (!strcmp(f.name, "car_data") && !s_wantCar) continue;
    uint32_t period = stream ? f.sync : f.period;
    if (!period) continue;
    uint32_t age = now - f.last;
    if (age >= period && age - period >= worst) { worst = age - period; due = &f; }
  }
  int st = 0;
  int K = s_cur.key;
  if (due) {
  due->last = now;

  char a[32], b[32], q[200];
  isoOf(dn, b, sizeof b);
  auto dated = [&](const char *ep, const char *field, int64_t cursor) {
    isoOf(cursor, a, sizeof a);
    if (bounded()) snprintf(q, sizeof q, "%s?session_key=%d&%s>%s&%s<=%s", ep, K, field, a, field, b);
    else snprintf(q, sizeof q, "%s?session_key=%d&%s>%s", ep, K, field, a);
  };
  F1Model &m = *s_model;
  const char *n = due->name;
  if (!strcmp(n, "race_control")) {
    dated(n, "date", m.curRc);
    st = fetch(q, [](const char *x, size_t l) { return s_model->ingestRaceControl(x, l); });
  } else if (!strcmp(n, "position")) {
    dated(n, "date", m.curPos);
    st = fetch(q, [](const char *x, size_t l) { return s_model->ingestPosition(x, l); });
  } else if (!strcmp(n, "intervals")) {
    dated(n, "date", m.curInt > dn - 60000 ? m.curInt : dn - 60000);
    st = fetch(q, [](const char *x, size_t l) { return s_model->ingestIntervals(x, l); });
  } else if (!strcmp(n, "laps")) {
    // Re-read the last five minutes: a lap's time arrives after its row does.
    dated(n, "date_start", dn - 300000);
    st = fetch(q, [dn](const char *x, size_t l) { return s_model->ingestLaps(x, l, dn); });
  } else if (!strcmp(n, "overtakes")) {
    dated(n, "date", m.curOvt);
    st = fetch(q, [](const char *x, size_t l) { return s_model->ingestOvertakes(x, l); });
  } else if (!strcmp(n, "pit")) {
    // Re-read a few minutes: a stop's durations are added to its row later.
    dated(n, "date", (m.curPit > dn - 600000 ? m.curPit : dn) - 240000);
    st = fetch(q, [](const char *x, size_t l) { return s_model->ingestPit(x, l); });
  } else if (!strcmp(n, "location")) {
    dated(n, "date", m.curLoc > dn - 2500 ? m.curLoc : dn - 2500);
    st = fetch(q, [](const char *x, size_t l) { return s_model->ingestLocation(x, l); });
  } else if (!strcmp(n, "car_data")) {
    isoOf(dn - 1500, a, sizeof a);
    if (bounded()) snprintf(q, sizeof q, "car_data?session_key=%d&driver_number=%d&date>%s&date<=%s", K, (int)s_wantCar, a, b);
    else snprintf(q, sizeof q, "car_data?session_key=%d&driver_number=%d&date>%s", K, (int)s_wantCar, a);
    st = fetch(q, [](const char *x, size_t l) { return s_model->ingestCarData(x, l); });
  } else if (!strcmp(n, "team_radio")) {
    dated(n, "date", m.curRadio);
    st = fetch(q, [](const char *x, size_t l) { return s_model->ingestTeamRadio(x, l); });
  } else if (!strcmp(n, "stints")) {
    snprintf(q, sizeof q, "stints?session_key=%d", K);
    st = fetch(q, [](const char *x, size_t l) { return s_model->ingestStints(x, l); });
  } else {
    dated(n, "date", m.curWx);
    st = fetch(q, [](const char *x, size_t l) { return s_model->ingestWeather(x, l); });
  }

  appLock(portMAX_DELAY);
  if (st == 401 || st == 403) s_status.authNeeded = true;
  else if (st == 200) s_status.authNeeded = false;
  appUnlock();
  } else {
    // A quiet moment: the portrait of the car on the driver screen, once.
    uint8_t car = s_wantCar;
    char url[sizeof(F1Driver::headUrl)] = "";
    if (headWanted(car)) {
      appLock(portMAX_DELAY);
      F1Driver *d = s_model->driver(car);
      if (d) strlcpy(url, d->headUrl, sizeof url);
      appUnlock();
      headLoad(car, url);
    }
    vTaskDelay(pdMS_TO_TICKS(stream ? 50 : 100));
  }
  if ((st == 401 || st == 403)) { stopLive(); s_liveRetryMs = millis(); return; }
  if (st == 200 && s_gapMs > POLL_GAP_MS && (int32_t)(millis() - s_backoffUntil) > 60000)
    s_gapMs = POLL_GAP_MS;                    // recover after a quiet minute

  // End of session.
  if (finished && !s_finishedMs) s_finishedMs = millis() | 1;
  if (isReplay()) {
    bool loop = REPLAY_LOOP_S > 0 && dn > s_cur.startMs + (int64_t)(REPLAY_OFFSET_S + REPLAY_LOOP_S) * 1000;
    if (loop || dn > s_cur.endMs + 5 * 60000 || (s_finishedMs && millis() - s_finishedMs > 120000)) {
      Serial.println("[app] replay finished — starting again");
      mqttStop();                             // a bench stream is still sending rows from "later"
      startLive(s_cur);
    }
  } else {
    int64_t wall = appWallMs();
    if (wall > s_cur.endMs + (int64_t)LIVE_TAIL_S * 1000 ||
        (s_finishedMs && millis() - s_finishedMs > 10 * 60000))
      stopLive(true);
  }
}

// ---- between sessions ----------------------------------------------------

static void idleRefresh() {
  int64_t wall = appWallMs();
  appLock(portMAX_DELAY);
  const F1SessionInfo *lr = s_model->lastEnded(wall, true);
  const F1SessionInfo *la = s_model->lastEnded(wall, false);
  int raceKey = lr ? lr->key : 0, anyKey = la ? la->key : 0;
  // A race that finished ahead of its scheduled end counts as ended.
  const F1SessionInfo *dk = s_doneKey ? s_model->sessionByKey(s_doneKey) : nullptr;
  if (dk && dk->endMs > wall) {
    anyKey = dk->key;
    if (dk->kind == F1_RACE) raceKey = dk->key;
  }
  int haveStand = s_model->info.standKey, haveRes = s_model->info.resultKey;
  appUnlock();
  char q[80];
  int n = 0;
  if (raceKey && haveStand != raceKey) {
    snprintf(q, sizeof q, "drivers?session_key=%d", raceKey);
    fetch(q, [](const char *b, size_t l) { return s_model->ingestWho(b, l); });
    snprintf(q, sizeof q, "championship_teams?session_key=%d", raceKey);
    fetch(q, [](const char *b, size_t l) { return s_model->ingestTeamStandings(b, l); });
    snprintf(q, sizeof q, "championship_drivers?session_key=%d", raceKey);
    fetch(q, [raceKey](const char *b, size_t l) { return s_model->ingestDriverStandings(b, l, raceKey); }, &n);
    Serial.printf("[app] standings after session %d: %d drivers\n", raceKey, n);
  }
  if (anyKey && haveRes != anyKey) {
    snprintf(q, sizeof q, "drivers?session_key=%d", anyKey);
    fetch(q, [](const char *b, size_t l) { return s_model->ingestWho(b, l); });
    snprintf(q, sizeof q, "session_result?session_key=%d", anyKey);
    fetch(q, [anyKey](const char *b, size_t l) { return s_model->ingestResult(b, l, anyKey); }, &n);
    Serial.printf("[app] result of session %d: %d rows\n", anyKey, n);
  }
  appLock(portMAX_DELAY);
  bool done = (!raceKey || s_model->info.standKey == raceKey) &&
              (!anyKey || s_model->info.resultKey == anyKey);
  appUnlock();
  s_idleForce = false;
  // Results can take a few minutes to be published after the flag: retry
  // soon until they are, then just check occasionally.
  s_idleMs = millis() - (done ? 0 : (10 * 60000 - 60000));
}

// ---- circuit outline -----------------------------------------------------

static uint32_t s_trackTryMs = 0;
static int      s_trackTryCircuit = 0;

// Trace the circuit from one car's fastest lap in a finished session there:
// this year's if the weekend has started, otherwise last year's race.
static void traceTrack(int circuitKey, int64_t wall) {
  s_trackTryMs = millis() | 1;
  s_trackTryCircuit = circuitKey;
  int key = 0;
  appLock(portMAX_DELAY);
  for (int i = 0; i < s_model->info.nSessions; ++i) {
    const F1SessionInfo &s = s_model->info.sessions[i];
    if (s.circuitKey == circuitKey && !s.cancelled && s.endMs + 20 * 60000 < wall) key = s.key;
  }
  appUnlock();
  char q[200];
  if (!key) {
    snprintf(q, sizeof q, "sessions?circuit_key=%d&year=%d&session_name=Race", circuitKey, s_calYear - 1);
    if (apiGet(q) == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, apiBody(), apiLen()) && doc.is<JsonArray>()) key = doc[0]["session_key"] | 0;
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_GAP_MS));
  }
  if (!key) { Serial.printf("[app] no past session at circuit %d to trace\n", circuitKey); return; }

  int drv = 0; int64_t t0 = 0; float dur = 0, s1 = 0, s2 = 0;
  for (int lap : {5, 3, 8}) {            // any one lap number: 20-odd rows, pick the quickest
    snprintf(q, sizeof q, "laps?session_key=%d&lap_number=%d", key, lap);
    int st = apiGet(q);
    vTaskDelay(pdMS_TO_TICKS(POLL_GAP_MS));
    if (st == 200 && F1Model::pickLap(apiBody(), apiLen(), &drv, &t0, &dur, &s1, &s2)) break;
    drv = 0;
    if (st == 401 || st == 403) return;
  }
  if (!drv) return;
  char a[32], b[32];
  f1FmtIso(t0, a, sizeof a);
  f1FmtIso(t0 + (int64_t)(dur * 1000) + 400, b, sizeof b);
  snprintf(q, sizeof q, "location?session_key=%d&driver_number=%d&date>=%s&date<=%s", key, drv, a, b);
  int n = 0;
  fetch(q, [=](const char *x, size_t l) { return s_model->ingestTrack(x, l, circuitKey, t0, s1, s2); }, &n);
  appLock(portMAX_DELAY);
  int b2 = s_model->info.trackSec2, b3 = s_model->info.trackSec3;
  appUnlock();
  Serial.printf("[app] circuit %d outline: %d points, sectors from %d and %d (session %d, car %d)\n", circuitKey, n,
                b2, b3, key, drv);
  if (n < 10) return;
  // Corner numbers and marshal sectors, placed on that outline. Another
  // host, so no login goes with it. One retry if the network let go.
  snprintf(q, sizeof q, CIRCUIT_INFO_URL, circuitKey, s_calYear);
  int st = apiGetUrl(q);
  if (st < 0) { vTaskDelay(pdMS_TO_TICKS(2000)); st = apiGetUrl(q); }
  int corners = 0, msec = 0;
  if (st == 200) {
    appLock(portMAX_DELAY);
    corners = s_model->ingestCircuit(apiBody(), apiLen(), circuitKey);
    msec = s_model->info.nMsec;
    appUnlock();
  }
  Serial.printf("[app] circuit %d info: http %d, %d corners, %d marshal sectors\n", circuitKey, st, corners, msec);
}

// ---- task ----------------------------------------------------------------

// Bench aid: "G<path?query>" on serial runs one GET from the net task and
// reports size, time and internal-heap low-water mark. See tools/serial_hub.py.
static char s_dbgPath[300];
void appDebugFetch(const char *pathQuery) { strlcpy(s_dbgPath, pathQuery, sizeof s_dbgPath); }

static void debugFetch() {
  char q[sizeof s_dbgPath];
  strlcpy(q, s_dbgPath, sizeof q);
  s_dbgPath[0] = 0;
  uint32_t t0 = millis();
  int st = apiGet(q);
  Serial.printf("[dbg] HTTP %d, %u bytes in %u ms; dma heap free %u, low-water %u, largest %u  (%s)\n",
                st, (unsigned)apiLen(), (unsigned)(millis() - t0),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA), q);
}

static volatile bool s_dbgStream = false;
void appDebugStream() { s_dbgStream = true; }

// Bench: prove the stream login without waiting for a session.
static void debugStream() {
  s_dbgStream = false;
  if (s_live) {
    Serial.printf("[dbg] stream is %s, %u rows so far\n", mqttUp() ? "up" : "DOWN", (unsigned)mqttRowCount());
    return;
  }
#ifdef MQTT_BENCH_HOST
  strcpy(s_tok, "bench");
#else
  if (!apiToken(s_tok, sizeof s_tok)) {
    Serial.println("[dbg] stream: no OpenF1 token (no credentials, or the login failed)");
    return;
  }
#endif
  mqttStop();
  mqttConnect(s_tok);
  uint32_t t0 = millis();
  while (!mqttUp() && mqttDownMs() != 0 && millis() - t0 < 20000) {
    if (millis() - t0 > 1000 && mqttDownMs() > 0 && mqttFailed()) break;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  bool ok = mqttUp();
  uint32_t took = millis() - t0;
  if (ok) vTaskDelay(pdMS_TO_TICKS(3000));    // anything being published?
  Serial.printf("[dbg] stream login %s after %u ms, %u rows; dma heap free %u, low-water %u\n",
                ok ? "OK" : "FAILED", (unsigned)took, (unsigned)mqttRowCount(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA));
  mqttStop();
}

static void netTask(void *) {
  setMsg("Connecting to Wi-Fi %s", WIFI_SSID);
  uint32_t wifiT0 = millis();
  for (;;) {
    bool up = WiFi.status() == WL_CONNECTED;
    int64_t wall = appWallMs();
    appLock(portMAX_DELAY);
    s_status.wifi = up;
    s_status.timeOk = wall != 0;
    appUnlock();
    if (!up) {
      if (millis() - wifiT0 > 20000) {
        setMsg("No Wi-Fi yet (%s) - retrying", WIFI_SSID);
        WiFi.reconnect();
        wifiT0 = millis();
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!wall) {
      static bool said = false;
      if (!said) { setMsg("Wi-Fi %s - setting the clock", WiFi.localIP().toString().c_str()); said = true; }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (s_dbgPath[0]) { debugFetch(); continue; }
    if (s_dbgStream) { debugStream(); continue; }

    // Calendar: at boot, then every six hours.
    if (!s_calYear || millis() - s_calMs > 6UL * 3600000UL) {
      int year = 0;
      if (isReplay()) year = replayYear();
      if (!year) { time_t t = wall / 1000; struct tm g; gmtime_r(&t, &g); year = g.tm_year + 1900; }
      if (!s_calYear) setMsg("Loading the %d calendar", year);
      if (!loadCalendar(year)) {
        if (!s_calYear) { setMsg("OpenF1 not reachable - retrying"); vTaskDelay(pdMS_TO_TICKS(10000)); continue; }
        s_calMs = millis() - 6UL * 3600000UL + 300000;   // keep the old one, retry in 5 min
      }
      if (s_status.mode == APP_BOOT) {
        appLock(portMAX_DELAY);
        s_status.mode = APP_IDLE;
        s_status.replay = isReplay();
        appUnlock();
        setMsg("%s", WiFi.localIP().toString().c_str());
      }
    }

    if (s_live) { pollOne(); continue; }

    // Is a session on (or about to be)?
    F1SessionInfo target;
    bool have = false;
    appLock(portMAX_DELAY);
    const F1SessionInfo *s = isReplay()
        ? s_model->sessionByKey(REPLAY_SESSION_KEY)
        : s_model->activeSession(wall - (int64_t)STREAM_DELAY_S * 1000,
                                 (int64_t)LIVE_LEAD_S * 1000, (int64_t)LIVE_TAIL_S * 1000);
    if (s) { target = *s; have = true; }
    appUnlock();
    if (have && target.key != s_doneKey && (!s_liveRetryMs || millis() - s_liveRetryMs > 30000)) {
      s_liveRetryMs = millis() | 1;
      if (startLive(target)) continue;
      if (s_status.authNeeded) setMsg("Live timing needs an OpenF1 login (see secrets.h)");
    }

    if (s_idleForce || millis() - s_idleMs > 10 * 60000) idleRefresh();

    // Outline of the next circuit for the countdown screen.
    int wantCircuit = 0, haveCircuit = 0;
    appLock(portMAX_DELAY);
    const F1SessionInfo *nx = s_model->nextSession(wall);
    if (nx) wantCircuit = nx->circuitKey;
    haveCircuit = s_model->info.trackCircuit;
    appUnlock();
    if (wantCircuit && wantCircuit != haveCircuit &&
        (s_trackTryCircuit != wantCircuit || millis() - s_trackTryMs > 15 * 60000))
      traceTrack(wantCircuit, wall);

    // Starting grid, once qualifying has set it (404 until then).
    int gridFor = 0, gridHave = 0;
    appLock(portMAX_DELAY);
    nx = s_model->nextSession(wall);
    if (nx && nx->kind == F1_RACE) gridFor = nx->meetingKey;
    gridHave = s_model->info.gridMeeting;
    appUnlock();
#ifdef GRID_TEST_MEETING
    gridFor = GRID_TEST_MEETING;
#endif
    static uint32_t gridTry = 0;
    if (gridFor && gridFor != gridHave && (!gridTry || millis() - gridTry > 5 * 60000)) {
      gridTry = millis() | 1;
      char q[64];
      int n = 0;
      snprintf(q, sizeof q, "starting_grid?meeting_key=%d", gridFor);
      fetch(q, [gridFor](const char *b, size_t l) { return s_model->ingestGrid(b, l, gridFor); }, &n);
      if (n > 0) Serial.printf("[app] starting grid of meeting %d: %d cars\n", gridFor, n);
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void appInit() {
  s_model = new F1Model();          // ~45 KB: heap (PSRAM), not .bss
  memset(&s_status, 0, sizeof s_status);
  s_status.mode = APP_BOOT;
  strlcpy(s_status.msg, "Starting", sizeof s_status.msg);
  s_mutex = xSemaphoreCreateMutex();
}

void appBegin() {
  apiBegin();
  mqttBegin(streamSink);
  xTaskCreateStaticPinnedToCore(netTask, "f1net", sizeof s_stack / sizeof s_stack[0], nullptr, 4,
                                s_stack, &s_tcb, ARDUINO_RUNNING_CORE);
}
