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
#include "settings.h"
#include "stamp.h"

static F1Model *s_model;
static AppStatus s_status;
static SemaphoreHandle_t s_mutex;

// Static task stack: internal RAM by construction (this core will otherwise
// put a large heap-allocated stack in PSRAM, which is slow for TLS).
static StackType_t s_stack[20 * 1024 / sizeof(StackType_t)];
static StaticTask_t s_tcb;

// waitMs: 0 = do not wait; portMAX_DELAY = wait for it. (pdMS_TO_TICKS() works
// in 32 bits, so handed portMAX_DELAY it would come out as 71 minutes.)
bool appLock(uint32_t waitMs) {
  TickType_t ticks = waitMs == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(waitMs);
  return s_mutex && xSemaphoreTake(s_mutex, ticks) == pdTRUE;
}
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
static uint32_t      s_calMs = 0, s_idleMs = 0, s_liveRetryMs = 0;
static uint32_t      s_backoffUntil = 0; // millis() deadline after an HTTP 429; 0 = not backing off
static uint32_t      s_finishedMs = 0;   // millis() when the race was seen to be finished; 0 = not yet
static uint32_t      s_stoppedMs = 0;    // millis() since when the session has not been under way; 0 = it is
static uint32_t      s_refusedMs = 0;    // millis() since when every live poll has been refused (401 / 403); 0 = they are not
static int           s_doneKey = 0;      // session already shown to its end
static bool          s_idleForce = true;
static uint32_t      s_lastReqMs = 0;    // millis() at the end of the newest API request
static bool          s_streamHeld = false;   // the stream's rows are collected, not applied yet: see holdStream()
static uint32_t      s_gapTryMs = 0;     // millis() of the newest fillGap() attempt
static uint32_t      s_owed = 0;         // bit per FeedId: that feed's whole history is still to be read

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
// The stream is delivering: its feeds only need the slow cross-check by REST.
static bool streamOk() { return streamWanted() && !s_streamHeld && mqttDownMs() < STREAM_GRACE_MS; }

static int64_t dataNow() {
  if (isReplay())
    return s_cur.startMs + (int64_t)REPLAY_OFFSET_S * 1000 +
           (int64_t)(millis() - s_replayT0) * REPLAY_SPEED;
  return appWallMs() - (int64_t)STREAM_DELAY_S * 1000;
}

// True while requests are paused after an HTTP 429. The deadline is cleared
// once it has passed: a millis() deadline left standing reads as "still in
// the future" again after 24.9 days of uptime.
static bool backingOff() {
  if (!s_backoffUntil) return false;
  if ((int32_t)(millis() - s_backoffUntil) < 0) return true;
  s_backoffUntil = 0;
  return false;
}

// Every API request goes through here: POLL_GAP_MS between any two of them,
// whoever asks. (This timer used to be a static inside fetch<>(), which the
// compiler makes once per call site: 27 timers, so no spacing between
// different requests.)
static int pacedGet(const char *q) {
  uint32_t since = millis() - s_lastReqMs;
  if (since < POLL_GAP_MS) vTaskDelay(pdMS_TO_TICKS(POLL_GAP_MS - since));
  int st = apiGet(q);
  s_lastReqMs = millis();
  return st;
}

// GET + ingest under the lock. Returns the HTTP status; *rows gets the count.
template <typename Fn>
static int fetch(const char *q, Fn ingest, int *rows = nullptr) {
  int st = pacedGet(q);
  int n = 0;
  if (st == 200) {
    appLock(portMAX_DELAY);
    n = ingest(apiBody(), apiLen());
    appUnlock();
    if (n < 0) Serial.printf("[app] bad JSON from %s\n", q);
  } else if (st == 429) {
    // Over the limit anyway (another client on the same account?): stay
    // silent for half a minute so the server's window can drain.
    s_backoffUntil = (millis() + 30000) | 1;
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
    case MT_RACE_CONTROL: m.ingestRaceControl(json, len, dn); break;
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

// The stream does not send again what was published while it was away - and
// it is away for 3-6 s every hour, when it logs in with a new token. Its
// first row afterwards would move the feed cursors past that stretch, and
// REST only ever asks for "after the cursor": a change of position or a flag
// in the gap was lost for good. So from the moment a connection is started
// its rows are collected but not applied, and the feeds are polled as if
// there were no stream, until fillGap() has read race control and positions
// from where the cursors stand. Then the collected rows go in, in order.
static void holdStream(bool on) {
  s_streamHeld = on;
  mqttHold(on);
}

// Keep the stream connected while a session is on. Each (re)connection logs
// in with a current token: they only last an hour.
static void streamKeep() {
  if (!streamWanted() || mqttUp()) return;
  static uint32_t last = 0;
  if (last && millis() - last < 2000) return;
  last = stampNow();
#ifdef MQTT_BENCH_HOST
  strcpy(s_tok, "bench");
  s_streamTokUntil = millis() + 0x3FFFFFFF;
#else
  if (!apiToken(s_tok, sizeof s_tok)) return;
  s_streamTokUntil = millis() + (uint32_t)apiTokenLeftMs();
#endif
  s_streamRowsMs = millis();
  holdStream(true);
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
#ifndef MQTT_BENCH_HOST
  // Not before the new token is in hand: this connection has two minutes
  // left, and a login that does not get through now is tried again.
  if (!quiet && !apiToken(s_tok, sizeof s_tok)) return;
#endif
  Serial.printf("[mqtt] %s - connecting afresh\n", old ? "login token about to expire" : "nothing received for a while");
  if (!s_streamHeld) mqttDrain();             // what has arrived is good: mqttStop() throws the rest away
  mqttStop();
}

// ---- calendar ------------------------------------------------------------

// 1 = loaded; 0 = the API answered but lists no sessions for that year;
// -1 = no usable answer.
static int loadCalendar(int year) {
  char q[64];
  int n = 0;
  snprintf(q, sizeof q, "sessions?year=%d", year);
  int st = fetch(q, [](const char *b, size_t l) { return s_model->ingestSessions(b, l); }, &n);
  if (st != 200 || n <= 0) return (st == 200 || st == 404) ? 0 : -1;
  snprintf(q, sizeof q, "meetings?year=%d", year);
  fetch(q, [](const char *b, size_t l) { return s_model->ingestMeetings(b, l); });
  s_calYear = year;
  s_calMs = millis();
  Serial.printf("[app] calendar %d: %d sessions\n", year, n);
  return 1;
}

// Replay needs the session's own year before the calendar can be loaded.
static int replayYear() {
  char q[48];
  snprintf(q, sizeof q, "sessions?session_key=%d", (int)REPLAY_SESSION_KEY);
  if (pacedGet(q) != 200) return 0;
  JsonDocument doc;
  if (deserializeJson(doc, apiBody(), apiLen()) || !doc.is<JsonArray>()) return 0;
  return doc[0]["year"] | 0;
}

// ---- live ----------------------------------------------------------------

static void traceTrack(int circuitKey, int64_t wall);

// period: polling on its own; sync: re-read interval while the stream is
// delivering the same feed (0 = not polled at all then).
enum FeedId : uint8_t {
  FD_RACE_CONTROL, FD_POSITION, FD_INTERVALS, FD_LAPS, FD_OVERTAKES, FD_PIT,
  FD_STINTS, FD_WEATHER, FD_LOCATION, FD_CAR_DATA, FD_TEAM_RADIO
};
struct Feed { FeedId id; const char *name; uint32_t period, sync; bool raceOnly; uint32_t last; };
static Feed s_feeds[] = {
    {FD_RACE_CONTROL, "race_control", POLL_FAST_MS, SYNC_FAST_MS, false, 0},
    {FD_POSITION, "position", POLL_FAST_MS, SYNC_FAST_MS, false, 0},
    {FD_INTERVALS, "intervals", POLL_FAST_MS, SYNC_SLOW_MS, true, 0},
    {FD_LAPS, "laps", POLL_LAPS_MS, SYNC_FAST_MS, false, 0},
    {FD_OVERTAKES, "overtakes", POLL_LAPS_MS, SYNC_PIT_MS, true, 0},
    {FD_PIT, "pit", POLL_PIT_MS, SYNC_PIT_MS, true, 0},
    {FD_STINTS, "stints", POLL_SLOW_MS, SYNC_SLOW_MS, false, 0},
    {FD_WEATHER, "weather", POLL_WX_MS, SYNC_WX_MS, false, 0},
    {FD_LOCATION, "location", POLL_LOC_MS, 0, false, 0},
    {FD_CAR_DATA, "car_data", POLL_CAR_MS, 0, false, 0},
    {FD_TEAM_RADIO, "team_radio", POLL_SLOW_MS, 0, false, 0},
};


static void stopLive(bool done = false) {
  if (done) s_doneKey = s_cur.key;
  mqttStop();
  holdStream(false);
  s_live = false;
  s_idleForce = true;
  appLock(portMAX_DELAY);
  s_status.mode = APP_IDLE;
  s_status.liveKey = 0;
  s_status.stream = false;
  appUnlock();
}

// Load everything a session has so far. False = nothing to show yet.
// late: the timetable has the session over (its end and the tail are past).
// It is joined only if race control still has it going - a delayed start or
// a red flag, and this panel rebooted meanwhile - and not asked about again
// once race control has it over.
static bool startLive(const F1SessionInfo &s, bool late = false) {
  // A session's stream starts after its model has been reset, never before:
  // streamKeep() below then holds its rows back (holdStream) until the
  // history and the stretch after it have been read. Race control is folded
  // in order: one pushed row landing first would move its cursor on, and
  // everything older (flags, "started", the qualifying segment) would be
  // skipped.
  mqttStop();
  holdStream(false);
  s_owed = 0;
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

  // Start the stream first, so nothing published while the history below is
  // being read is missed; its rows wait (holdStream) and every ingest is
  // safe to repeat. (A late join first has to see whether there is anything
  // to join.)
  if (!late) streamKeep();

  // A history read that fails here is not lost. Race control and positions
  // are read from their cursors, which then still stand at the beginning
  // (and fillGap() insists on both before any stream row goes in); stints
  // are always read whole; laps and pit stops are only re-read a few minutes
  // back, so those two are marked as owed and pollFeed() asks for the lot
  // until it gets it.
  auto got = [](int status) { return status == 200 || status == 404; };

  if (bounded()) snprintf(q, sizeof q, "race_control?session_key=%d&date<=%s", K, iso);
  else snprintf(q, sizeof q, "race_control?session_key=%d", K);
  st = fetch(q, [dn](const char *b, size_t l) { return s_model->ingestRaceControl(b, l, dn); });

  if (late) {
    appLock(portMAX_DELAY);
    F1Progress p = s_model->progress();
    appUnlock();
    // A break between qualifying segments is joined too: the next one is
    // minutes away. Not started, or no answer: asked again later.
    if (p != PROG_UNDER_WAY && p != PROG_BREAK) {
      if (got(st) && p == PROG_OVER) s_doneKey = K;
      return false;
    }
    Serial.printf("[app] session %d is running past its timetable - joining it\n", K);
    streamKeep();
  }

  if (bounded()) snprintf(q, sizeof q, "position?session_key=%d&date<=%s", K, iso);
  else snprintf(q, sizeof q, "position?session_key=%d", K);
  fetch(q, [](const char *b, size_t l) { return s_model->ingestPosition(b, l); });

  // All laps so far. (A date filter would drop race lap 1, which has no date.)
  snprintf(q, sizeof q, "laps?session_key=%d", K);
  if (!got(fetch(q, [dn](const char *b, size_t l) { return s_model->ingestLaps(b, l, dn); }))) s_owed |= 1u << FD_LAPS;

  snprintf(q, sizeof q, "stints?session_key=%d", K);
  fetch(q, [](const char *b, size_t l) { return s_model->ingestStints(b, l); });

  if (s.kind == F1_RACE) {                    // every stop so far (a few dozen rows)
    if (bounded()) snprintf(q, sizeof q, "pit?session_key=%d&date<=%s", K, iso);
    else snprintf(q, sizeof q, "pit?session_key=%d", K);
    if (!got(fetch(q, [](const char *b, size_t l) { return s_model->ingestPit(b, l); }))) s_owed |= 1u << FD_PIT;
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
  s_stoppedMs = 0;
  s_refusedMs = 0;
  Serial.printf("[app] live: session %d (%s), %d drivers\n", K, s.name, n);
  return true;
}

struct LiveTick {
  bool raceFinished;   // a race has had its chequered flag
  bool underWay;       // race control has started the session, not ended it, and rows are still coming
};

// Runs on every pass of the live loop, whether or not a poll is due.
static LiveTick liveTick(int64_t dn) {
  streamCheck();
  streamKeep();
  if (!s_streamHeld) mqttDrain();
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
  F1Model &m = *s_model;
  m.flushOvertakes(dn);
  s_status.dataNowMs = dn;
  s_status.stream = mqttUp() && !s_streamHeld;
  LiveTick t;
  t.raceFinished = m.live.finished && m.live.kind == F1_RACE;
  t.underWay = m.live.started && !m.live.finished && dn - m.newestRowMs() < 5 * 60000;
  appUnlock();
  return t;
}

// One REST read of a feed, from where its cursor stands. Returns the HTTP status.
static int pollFeed(const Feed &f, int64_t dn) {
  const int K = s_cur.key;
  // The MQTT task moves the cursors on as rows arrive: copy them under the lock.
  appLock(portMAX_DELAY);
  const F1Model &m = *s_model;
  const int64_t curRc = m.curRc, curPos = m.curPos, curInt = m.curInt, curOvt = m.curOvt,
                curPit = m.curPit, curLoc = m.curLoc, curRadio = m.curRadio, curWx = m.curWx;
  appUnlock();

  char a[32], b[32], q[200];
  isoOf(dn, b, sizeof b);
  auto dated = [&](const char *field, int64_t cursor) {
    isoOf(cursor, a, sizeof a);
    if (bounded()) snprintf(q, sizeof q, "%s?session_key=%d&%s>%s&%s<=%s", f.name, K, field, a, field, b);
    else snprintf(q, sizeof q, "%s?session_key=%d&%s>%s", f.name, K, field, a);
  };
  const bool owed = s_owed & (1u << f.id);     // the read of its whole history at the session's start failed
  auto settle = [&](int st) {
    if (owed && (st == 200 || st == 404)) s_owed &= ~(1u << f.id);
    return st;
  };
  switch (f.id) {
    case FD_RACE_CONTROL:
      // A little before the cursor: a flag row can be dated before a status
      // row that was sent first (F1_RC_REORDER_MS). The model knows a repeat.
      dated("date", curRc > F1_RC_REORDER_MS ? curRc - F1_RC_REORDER_MS : curRc);
      return fetch(q, [dn](const char *x, size_t l) { return s_model->ingestRaceControl(x, l, dn); });
    case FD_POSITION:
      // The newest row wins per car, so a stretch read twice costs nothing:
      // as the stream's cross-check, go back one period for rows that REST
      // listed late.
      dated("date", streamOk() && curPos > SYNC_FAST_MS ? curPos - SYNC_FAST_MS : curPos);
      return fetch(q, [](const char *x, size_t l) { return s_model->ingestPosition(x, l); });
    case FD_INTERVALS:
      dated("date", curInt > dn - 60000 ? curInt : dn - 60000);
      return fetch(q, [](const char *x, size_t l) { return s_model->ingestIntervals(x, l); });
    case FD_LAPS:
      // Re-read the last five minutes: a lap's time arrives after its row does.
      if (owed) snprintf(q, sizeof q, "laps?session_key=%d", K);
      else dated("date_start", dn - 300000);
      return settle(fetch(q, [dn](const char *x, size_t l) { return s_model->ingestLaps(x, l, dn); }));
    case FD_OVERTAKES:
      dated("date", curOvt);
      return fetch(q, [](const char *x, size_t l) { return s_model->ingestOvertakes(x, l); });
    case FD_PIT:
      // Re-read a few minutes: a stop's durations are added to its row later.
      dated("date", owed ? 0 : (curPit > dn - 600000 ? curPit : dn) - 240000);
      return settle(fetch(q, [](const char *x, size_t l) { return s_model->ingestPit(x, l); }));
    case FD_LOCATION:
      dated("date", curLoc > dn - 2500 ? curLoc : dn - 2500);
      return fetch(q, [](const char *x, size_t l) { return s_model->ingestLocation(x, l); });
    case FD_CAR_DATA: {
      const int car = s_wantCar;
      isoOf(dn - 1500, a, sizeof a);
      if (bounded()) snprintf(q, sizeof q, "car_data?session_key=%d&driver_number=%d&date>%s&date<=%s", K, car, a, b);
      else snprintf(q, sizeof q, "car_data?session_key=%d&driver_number=%d&date>%s", K, car, a);
      return fetch(q, [](const char *x, size_t l) { return s_model->ingestCarData(x, l); });
    }
    case FD_TEAM_RADIO:
      dated("date", curRadio);
      return fetch(q, [](const char *x, size_t l) { return s_model->ingestTeamRadio(x, l); });
    case FD_STINTS:
      snprintf(q, sizeof q, "stints?session_key=%d", K);
      return fetch(q, [](const char *x, size_t l) { return s_model->ingestStints(x, l); });
    case FD_WEATHER:
      dated("date", curWx);
      return fetch(q, [](const char *x, size_t l) { return s_model->ingestWeather(x, l); });
  }
  return 0;
}

// The stream is connected and its rows are waiting (holdStream): read by
// REST what it will not send again, then let them in. Both reads have to be
// answered; if not, the stream stays held, the feeds go on being polled as
// if there were no stream, and this is tried again a few seconds later.
static bool fillGap(int64_t dn) {
  for (FeedId id : {FD_RACE_CONTROL, FD_POSITION}) {
    Feed *feed = nullptr;
    for (Feed &f : s_feeds)
      if (f.id == id) feed = &f;
    if (!feed) continue;
    feed->last = millis();
    int st = pollFeed(*feed, dn);
    if (st != 200 && st != 404) return false;
  }
  holdStream(false);
  mqttDrain();
  return true;
}

// The portrait of the car on the driver screen, once, in a quiet moment.
static void loadPortrait() {
  uint8_t car = s_wantCar;
  if (!headWanted(car)) return;
  char url[sizeof(F1Driver::headUrl)] = "";
  appLock(portMAX_DELAY);
  const F1Driver *d = s_model->driver(car);
  if (d) strlcpy(url, d->headUrl, sizeof url);
  appUnlock();
  headLoad(car, url);
}

static void pollOne() {
  uint32_t now = millis();
  int64_t dn = dataNow();
  LiveTick tick = liveTick(dn);
  if (backingOff()) { vTaskDelay(pdMS_TO_TICKS(250)); return; }
  bool stream = streamOk();
  Feed *due = nullptr;
  uint32_t worst = 0;
  for (Feed &f : s_feeds) {
    if (f.raceOnly && s_cur.kind != F1_RACE) continue;
    if (f.id == FD_OVERTAKES && !OVERTAKE_TOASTS) continue;
    if (f.id == FD_LOCATION && !s_wantLoc) continue;
    if (f.id == FD_CAR_DATA && !s_wantCar) continue;
    uint32_t period = stream ? f.sync : f.period;
    if (!period) continue;
    uint32_t age = now - f.last;
    if (age >= period && age - period >= worst) { worst = age - period; due = &f; }
  }
  if (s_streamHeld && mqttUp() && millis() - s_gapTryMs > 3000) {
    s_gapTryMs = millis();
    if (fillGap(dn)) Serial.println("[mqtt] caught up by REST - stream rows go in from here");
  } else if (due) {
    due->last = now;
    int st = pollFeed(*due, dn);
    if (st == 401 || st == 403) {
      // A refusal can be a token that ran out while its renewal did not get
      // through; f1_api tries that again within seconds. Only when every
      // poll has been refused for AUTH_REFUSED_MS is the login taken to be
      // no good: leave the live screen and say so - unless the stream is
      // connected, which the same login got: then stay, and let its rows in
      // without waiting any longer for REST.
      if (!s_refusedMs) s_refusedMs = stampNow();
      else if (millis() - s_refusedMs > AUTH_REFUSED_MS) {
        if (mqttUp()) {
          if (s_streamHeld) holdStream(false);
        } else {
          appLock(portMAX_DELAY);
          s_status.authNeeded = true;
          appUnlock();
          stopLive();
          s_liveRetryMs = stampNow();
          return;
        }
      }
    } else if (st == 200 || st == 404) {      // 404 = nothing new, which only a login gets while a session is on
      s_refusedMs = 0;
      appLock(portMAX_DELAY);
      s_status.authNeeded = false;
      appUnlock();
    }
  } else {
    loadPortrait();
    vTaskDelay(pdMS_TO_TICKS(stream ? 50 : 100));
  }

  // End of session.
  if (tick.raceFinished && !s_finishedMs) s_finishedMs = stampNow();
  if (isReplay()) {
    bool loop = REPLAY_LOOP_S > 0 && dn > s_cur.startMs + (int64_t)(REPLAY_OFFSET_S + REPLAY_LOOP_S) * 1000;
    if (loop || dn > s_cur.endMs + 5 * 60000 || (s_finishedMs && millis() - s_finishedMs > 120000)) {
      Serial.println("[app] replay finished — starting again");
      mqttStop();                             // a bench stream is still sending rows from "later"
      // Nothing loaded (the API did not answer): back to idle, which tries
      // again, rather than polling on with an empty driver list.
      if (!startLive(s_cur)) stopLive();
    }
    return;
  }
  // A race: ten minutes after its flag. Otherwise the timetable's end plus
  // the tail - unless the session is plainly still going (a delayed start, a
  // red flag): then stay with it, up to LIVE_OVERRUN_MAX_S past the
  // timetable. "Not under way" has to last LIVE_OVERRUN_GRACE_S before it
  // counts, because qualifying is "finished" between its segments.
  if (tick.underWay) s_stoppedMs = 0;
  else if (!s_stoppedMs) s_stoppedMs = stampNow();
  int64_t wall = appWallMs();
  bool pastTail = wall > s_cur.endMs + (int64_t)LIVE_TAIL_S * 1000;
  bool pastLimit = wall > s_cur.endMs + (int64_t)LIVE_OVERRUN_MAX_S * 1000;
  bool stillOn = !s_stoppedMs || millis() - s_stoppedMs < LIVE_OVERRUN_GRACE_S * 1000UL;
  if ((pastTail && (pastLimit || !stillOn)) || (s_finishedMs && millis() - s_finishedMs > 10 * 60000))
    stopLive(true);
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
  s_trackTryMs = stampNow();
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
    if (pacedGet(q) == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, apiBody(), apiLen()) && doc.is<JsonArray>()) key = doc[0]["session_key"] | 0;
    }
  }
  if (!key) { Serial.printf("[app] no past session at circuit %d to trace\n", circuitKey); return; }

  int drv = 0; int64_t t0 = 0; float dur = 0, s1 = 0, s2 = 0;
  for (int lap : {5, 3, 8}) {            // any one lap number: 20-odd rows, pick the quickest
    snprintf(q, sizeof q, "laps?session_key=%d&lap_number=%d", key, lap);
    int st = pacedGet(q);
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
static volatile bool s_dbgFetch = false;
// Called from the serial (loop) task. The flag goes up after the path is
// whole, and a second command waits its turn, so the net task never reads a
// half-written path.
void appDebugFetch(const char *pathQuery) {
  if (s_dbgFetch) return;
  strlcpy(s_dbgPath, pathQuery, sizeof s_dbgPath);
  s_dbgFetch = true;
}

static void debugFetch() {
  char q[sizeof s_dbgPath];
  strlcpy(q, s_dbgPath, sizeof q);
  s_dbgFetch = false;
  uint32_t t0 = millis();
  int st = pacedGet(q);
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
  const char *ssid = settings().wifiSsid;
  if (ssid[0]) setMsg("Connecting to Wi-Fi %s", ssid);
  else setMsg("Set-up: plug in USB and open " SETUP_URL);
  uint32_t wifiT0 = millis();
  for (;;) {
    bool up = WiFi.status() == WL_CONNECTED;
    int64_t wall = appWallMs();
    appLock(portMAX_DELAY);
    s_status.wifi = up;
    s_status.timeOk = wall != 0;
    appUnlock();
    if (!up) {
      if (ssid[0] && millis() - wifiT0 > 20000) {
        setMsg("No Wi-Fi yet (%s) - retrying", ssid);
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

    if (s_dbgFetch) { debugFetch(); continue; }
    if (s_dbgStream) { debugStream(); continue; }

    // Calendar: at boot, then every six hours.
    if (!s_calYear || millis() - s_calMs > 6UL * 3600000UL) {
      int year = 0;
      if (isReplay()) year = replayYear();
      if (!year) { time_t t = wall / 1000; struct tm g; gmtime_r(&t, &g); year = g.tm_year + 1900; }
      if (!s_calYear) setMsg("Loading the %d calendar", year);
      int got = loadCalendar(year);
      // In January the new season may not be listed yet: last year's calendar
      // still gives the final standings and results, and the new one is asked
      // for again below.
      if (got == 0 && !s_calYear && !isReplay()) loadCalendar(year - 1);   // sets s_calYear if it loads
      if (got != 1) {
        if (!s_calYear) {
          if (got == 0) setMsg("No calendar for %d yet - retrying", year);
          else setMsg("OpenF1 not reachable - retrying");
          vTaskDelay(pdMS_TO_TICKS(10000));
          continue;
        }
        s_calMs = millis() - 6UL * 3600000UL + 300000;   // keep the one in hand, ask again in 5 min
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
    bool have = false, late = false;
    appLock(portMAX_DELAY);
    const F1SessionInfo *s = nullptr;
    if (isReplay()) {
      s = s_model->sessionByKey(REPLAY_SESSION_KEY);
    } else {
      const int64_t now = wall - (int64_t)STREAM_DELAY_S * 1000;
      s = s_model->activeSession(now, (int64_t)LIVE_LEAD_S * 1000, (int64_t)LIVE_TAIL_S * 1000);
      if (!s) {
        // Nothing on by the timetable. The session that ended last may still
        // be running past it without this panel showing it: after a reboot,
        // or after the live screen was left for a refused login. Only that
        // one session is looked at, so a later session's own window always
        // comes first; startLive() decides whether there is anything to join.
        s = s_model->lastEnded(now, false);
        if (s && now <= s->endMs + (int64_t)LIVE_OVERRUN_MAX_S * 1000) late = true;
        else s = nullptr;
      }
    }
    if (s) { target = *s; have = true; }
    appUnlock();
    const uint32_t retryMs = late ? 120000 : 30000;
    if (have && target.key != s_doneKey && (!s_liveRetryMs || millis() - s_liveRetryMs > retryMs)) {
      s_liveRetryMs = stampNow();
      if (startLive(target, late)) continue;
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
      gridTry = stampNow();
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
