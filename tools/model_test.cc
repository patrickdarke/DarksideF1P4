// Desktop unit tests of f1_model: small hand-written responses, pass / fail.
//   c++ -std=c++17 -Wall -I lib/ArduinoJson/src -I . tools/model_test.cc f1_model.cpp -o /tmp/model_test && /tmp/model_test
// Needs no saved samples (host_test.cc replays whole saved sessions).
// The cases are the ones a stream makes possible and a REST poll alone does
// not: one row at a time, the same row twice, a newer row before an older one.
#include "f1_model.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0, g_run = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_run;                                                                     \
    if (!(cond)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

static const char *DRIVERS =
    "[{\"driver_number\":44,\"name_acronym\":\"HAM\",\"last_name\":\"Hamilton\",\"team_name\":\"Ferrari\",\"team_colour\":\"E80020\"},"
    "{\"driver_number\":1,\"name_acronym\":\"NOR\",\"last_name\":\"Norris\",\"team_name\":\"McLaren\",\"team_colour\":\"FF8000\"}]";

static F1Model *fresh(F1Kind kind) {
  F1Model *m = new F1Model;
  m->resetLive(100, kind);
  m->ingestDrivers(DRIVERS, strlen(DRIVERS));
  return m;
}
static int pos(F1Model &m, const char *j) { return m.ingestPosition(j, strlen(j)); }
static int laps(F1Model &m, const char *j, int64_t now) { return m.ingestLaps(j, strlen(j), now); }
static int rc(F1Model &m, const char *j) { return m.ingestRaceControl(j, strlen(j)); }

static void testIso() {
  char b[40];
  f1FmtIso(f1ParseIso("2026-11-22T04:50:03.173000+00:00"), b, sizeof b);
  CHECK(!strcmp(b, "2026-11-22T04:50:03.173"));
  CHECK(f1ParseIso("2026-11-22T04:50:03+01:00") == f1ParseIso("2026-11-22T03:50:03"));
  // Short forms are refused instead of being read past their end.
  CHECK(f1ParseIso("2026-1-5T1:2:3") == 0);
  CHECK(f1ParseIso("2026-11-22") == 0);
  CHECK(f1ParseIso("") == 0);
  CHECK(f1ParseIso(nullptr) == 0);
}

// The grid slot is the session's earliest position row, in whatever order
// the rows arrive.
static void testGridOrder() {
  const char *newer = "[{\"date\":\"2026-11-22T04:10:00\",\"session_key\":100,\"driver_number\":44,\"position\":3}]";
  const char *history =
      "[{\"date\":\"2026-11-22T03:55:00\",\"session_key\":100,\"driver_number\":44,\"position\":5},"
      "{\"date\":\"2026-11-22T04:10:00\",\"session_key\":100,\"driver_number\":44,\"position\":3}]";
  F1Model *a = fresh(F1_RACE);          // the stream's row first, then the history
  pos(*a, newer);
  pos(*a, history);
  F1Model *b = fresh(F1_RACE);          // the history first
  pos(*b, history);
  pos(*b, newer);
  for (F1Model *m : {a, b}) {
    const F1Driver *d = m->driver(44);
    CHECK(d && d->pos == 3);
    CHECK(d && d->gridPos == 5);
    CHECK(m->curPos == f1ParseIso("2026-11-22T04:10:00"));
  }
  uint32_t v = a->live.version;
  CHECK(pos(*a, history) == 1);         // the newest row is applied again, the old one is not
  CHECK(a->live.version == v);          // and nothing changed
  // Rows of another session (the stream can carry its tail) are ignored.
  CHECK(pos(*a, "[{\"date\":\"2026-11-22T04:20:00\",\"session_key\":99,\"driver_number\":44,\"position\":9}]") == 0);
  CHECK(a->driver(44)->pos == 3);
  delete a;
  delete b;
}

// A late copy of an older lap, arriving on its own, does not replace the
// newest lap's time.
static void testLastLap() {
  F1Model *m = fresh(F1_RACE);
  int64_t now = f1ParseIso("2026-11-22T05:00:00");
  const char *lap12 = "[{\"session_key\":100,\"driver_number\":44,\"lap_number\":12,\"date_start\":\"2026-11-22T04:30:00\",\"lap_duration\":90.0}]";
  const char *lap11 = "[{\"session_key\":100,\"driver_number\":44,\"lap_number\":11,\"date_start\":\"2026-11-22T04:28:25\",\"lap_duration\":95.0}]";
  laps(*m, lap12, now);
  laps(*m, lap11, now);
  const F1Driver *d = m->driver(44);
  CHECK(d->lastLap == 90.0f);
  CHECK(d->lastLapNo == 12);
  CHECK(d->lapNo == 12);
  CHECK(d->best[0] == 90.0f);
  laps(*m, lap12, now);                 // and the same row again changes nothing
  CHECK(d->lastLap == 90.0f && d->best[0] == 90.0f);
  // A lap still being driven at "now" (replay) is not shown yet.
  const char *lap13 = "[{\"session_key\":100,\"driver_number\":44,\"lap_number\":13,\"date_start\":\"2026-11-22T04:59:30\",\"lap_duration\":88.0}]";
  laps(*m, lap13, now);
  CHECK(d->lapNo == 13);
  CHECK(d->lastLap == 90.0f);
  delete m;
}

// Race control: a repeat is ignored; the fixed-size tables hold.
static void testRaceControl() {
  F1Model *m = fresh(F1_RACE);
  m->alertsArmed = true;
  const char *sc =
      "[{\"date\":\"2026-11-22T04:00:00\",\"session_key\":100,\"category\":\"SessionStatus\",\"message\":\"SESSION STARTED\"},"
      "{\"date\":\"2026-11-22T04:36:00\",\"session_key\":100,\"category\":\"SafetyCar\",\"message\":\"SAFETY CAR DEPLOYED\"}]";
  CHECK(rc(*m, sc) == 2);
  CHECK(m->live.started && m->live.track == TRK_SC);
  CHECK(rc(*m, sc) == 0);               // polled again, or pushed after it was polled
  int alerts = 0;
  F1Alert a;
  while (m->popAlert(a)) ++alerts;
  CHECK(alerts == 2);                   // lights out, safety car: once each
  // Marshal sectors: 1..32 have a bit, anything else is ignored.
  rc(*m, "[{\"date\":\"2026-11-22T04:37:00\",\"session_key\":100,\"category\":\"Flag\",\"flag\":\"YELLOW\",\"scope\":\"Sector\",\"sector\":32,\"message\":\"YELLOW IN TRACK SECTOR 32\"},"
         "{\"date\":\"2026-11-22T04:37:01\",\"session_key\":100,\"category\":\"Flag\",\"flag\":\"YELLOW\",\"scope\":\"Sector\",\"sector\":33,\"message\":\"YELLOW IN TRACK SECTOR 33\"},"
         "{\"date\":\"2026-11-22T04:37:02\",\"session_key\":100,\"category\":\"Flag\",\"flag\":\"DOUBLE YELLOW\",\"scope\":\"Sector\",\"sector\":0,\"message\":\"x\"}]");
  CHECK(m->live.yellowMask == 0x80000000u);
  rc(*m, "[{\"date\":\"2026-11-22T04:38:00\",\"session_key\":100,\"category\":\"Flag\",\"flag\":\"CLEAR\",\"scope\":\"Sector\",\"sector\":32,\"message\":\"CLEAR IN TRACK SECTOR 32\"}]");
  CHECK(m->live.yellowMask == 0);
  CHECK(m->newestRowMs() == f1ParseIso("2026-11-22T04:38:00"));
  delete m;
}

// Race-control rows are not in date order: a flag row is stamped to the whole
// second and sent after a status row stamped 90 ms later (rows as the feed
// has them, Las Vegas qualifying 2026).
static void testRaceControlOrder() {
  const char *fin = "{\"date\":\"2026-11-21T04:18:00.090000+00:00\",\"session_key\":100,\"category\":\"SessionStatus\",\"flag\":null,\"scope\":null,\"message\":\"SESSION FINISHED\"}";
  const char *cheq = "{\"date\":\"2026-11-21T04:18:00+00:00\",\"session_key\":100,\"category\":\"Flag\",\"flag\":\"CHEQUERED\",\"scope\":\"Track\",\"message\":\"CHEQUERED FLAG\"}";
  std::string both = std::string("[") + fin + "," + cheq + "]";
  std::string one1 = std::string("[") + fin + "]", one2 = std::string("[") + cheq + "]";

  F1Model *a = fresh(F1_QUALI);         // one REST answer with both
  CHECK(rc(*a, both.c_str()) == 2);
  CHECK(a->live.track == TRK_FINISHED && a->live.finished);
  CHECK(rc(*a, both.c_str()) == 0);     // read again (REST asks a little before the cursor): nothing new
  delete a;

  F1Model *b = fresh(F1_QUALI);         // the stream: one row at a time
  CHECK(rc(*b, one1.c_str()) == 1);
  CHECK(rc(*b, one2.c_str()) == 1);
  CHECK(b->live.track == TRK_FINISHED);
  CHECK(b->curRc == f1ParseIso("2026-11-21T04:18:00.090"));   // the cursor does not go back
  // Older than the window: dropped, as before.
  CHECK(rc(*b, "[{\"date\":\"2026-11-21T04:17:50+00:00\",\"session_key\":100,\"category\":\"Flag\",\"flag\":\"RED\",\"scope\":\"Track\",\"message\":\"RED FLAG\"}]") == 0);
  CHECK(b->live.track == TRK_FINISHED);
  delete b;
}

// "Is the session over?" for a panel that boots after the timetable's end.
// Qualifying is finished three times; rows as the feed tags them (Las Vegas
// 2026): a flag in the break after Q1 already says phase 2.
static void testProgress() {
  auto row = [](const char *date, const char *cat, const char *flag, const char *msg, int phase) {
    char b[320];
    snprintf(b, sizeof b, "[{\"date\":\"%s\",\"session_key\":100,\"category\":\"%s\",\"flag\":%s%s%s,\"scope\":\"Track\","
             "\"qualifying_phase\":%d,\"message\":\"%s\"}]", date, cat, flag ? "\"" : "", flag ? flag : "null", flag ? "\"" : "", phase, msg);
    return std::string(b);
  };
  F1Model *m = fresh(F1_QUALI);
  CHECK(m->progress() == PROG_NOT_STARTED);
  rc(*m, row("2026-11-21T04:00:00.092", "SessionStatus", nullptr, "SESSION STARTED", 1).c_str());
  CHECK(m->progress() == PROG_UNDER_WAY);
  rc(*m, row("2026-11-21T04:18:00.090", "SessionStatus", nullptr, "SESSION FINISHED", 1).c_str());
  rc(*m, row("2026-11-21T04:18:00", "Flag", "CHEQUERED", "CHEQUERED FLAG", 1).c_str());
  CHECK(m->progress() == PROG_BREAK);
  rc(*m, row("2026-11-21T04:24:43", "Flag", "YELLOW", "YELLOW FLAG", 2).c_str());   // in the break, tagged Q2
  CHECK(m->progress() == PROG_BREAK);
  rc(*m, row("2026-11-21T04:27:00.097", "SessionStatus", nullptr, "SESSION STARTED", 2).c_str());
  CHECK(m->progress() == PROG_UNDER_WAY);
  rc(*m, row("2026-11-21T04:42:00.093", "SessionStatus", nullptr, "SESSION FINISHED", 2).c_str());
  rc(*m, row("2026-11-21T04:45:00", "Flag", "YELLOW", "YELLOW FLAG", 3).c_str());   // tagged Q3 before Q3 has begun
  CHECK(m->live.qPhase == 3 && m->progress() == PROG_BREAK);
  rc(*m, row("2026-11-21T04:49:00.097", "SessionStatus", nullptr, "SESSION STARTED", 3).c_str());
  rc(*m, row("2026-11-21T05:02:00.104", "SessionStatus", nullptr, "SESSION FINISHED", 3).c_str());
  CHECK(m->progress() == PROG_OVER);
  delete m;

  m = fresh(F1_QUALI);                  // no segment numbers in the feed: never "over" on this evidence
  rc(*m, row("2026-11-21T05:02:00.104", "SessionStatus", nullptr, "SESSION FINISHED", 0).c_str());
  CHECK(m->progress() == PROG_BREAK);
  delete m;

  for (F1Kind k : {F1_PRACTICE, F1_RACE}) {
    m = fresh(k);
    rc(*m, row("2026-11-22T04:00:00", "SessionStatus", nullptr, "SESSION STARTED", 0).c_str());
    CHECK(m->progress() == PROG_UNDER_WAY);
    rc(*m, row("2026-11-22T05:30:00", "Flag", "CHEQUERED", "CHEQUERED FLAG", 0).c_str());
    CHECK(m->progress() == PROG_OVER);
    delete m;
  }
}

// An old row changes the state without being announced.
static void testStaleAlert() {
  const char *sc = "[{\"date\":\"2026-11-22T04:36:00\",\"session_key\":100,\"category\":\"SafetyCar\",\"message\":\"SAFETY CAR DEPLOYED\"}]";
  F1Alert x;
  F1Model *m = fresh(F1_RACE);
  m->alertsArmed = true;
  CHECK(m->ingestRaceControl(sc, strlen(sc), f1ParseIso("2026-11-22T05:10:00")) == 1);   // read 34 minutes late
  CHECK(m->live.track == TRK_SC);
  CHECK(!m->popAlert(x));
  delete m;
  m = fresh(F1_RACE);
  m->alertsArmed = true;
  CHECK(m->ingestRaceControl(sc, strlen(sc), f1ParseIso("2026-11-22T04:36:04")) == 1);   // as it happens
  CHECK(m->popAlert(x) && x.kind == ALERT_SC);
  delete m;
}

// An older copy of the lap being driven does not take a sector back.
static void testSectorRollback() {
  F1Model *m = fresh(F1_RACE);
  int64_t now = f1ParseIso("2026-11-22T05:00:00");
  const char *s12 = "[{\"session_key\":100,\"driver_number\":44,\"lap_number\":12,\"date_start\":\"2026-11-22T04:58:00\",\"duration_sector_1\":30.1,\"duration_sector_2\":31.2}]";
  const char *s1 = "[{\"session_key\":100,\"driver_number\":44,\"lap_number\":12,\"date_start\":\"2026-11-22T04:58:00\",\"duration_sector_1\":30.1}]";
  laps(*m, s12, now);
  laps(*m, s1, now);                    // the REST answer that was already on its way
  const F1Driver *d = m->driver(44);
  CHECK(d->sector[0] == 30.1f && d->sector[1] == 31.2f);
  // The next lap starts with one sector, and that does replace the old lap's.
  laps(*m, "[{\"session_key\":100,\"driver_number\":44,\"lap_number\":13,\"date_start\":\"2026-11-22T04:59:20\",\"duration_sector_1\":29.9}]", now);
  CHECK(d->secLap == 13 && d->sector[0] == 29.9f && d->sector[1] == 0);
  delete m;
}

// A flag is handed out ahead of the strips that were queued before it.
static void testAlertOrder() {
  F1Model *m = fresh(F1_RACE);
  m->alertsArmed = true;
  rc(*m, "[{\"date\":\"2026-11-22T04:00:00\",\"session_key\":100,\"category\":\"SessionStatus\",\"message\":\"SESSION STARTED\"}]");
  F1Alert x;
  CHECK(m->popAlert(x) && x.kind == ALERT_START);
  const char *pits = "[{\"date\":\"2026-11-22T04:30:00\",\"session_key\":100,\"driver_number\":44,\"lap_number\":20,\"lane_duration\":22.4},"
                     "{\"date\":\"2026-11-22T04:30:02\",\"session_key\":100,\"driver_number\":1,\"lap_number\":20,\"lane_duration\":23.0}]";
  CHECK(m->ingestPit(pits, strlen(pits)) == 2);
  rc(*m, "[{\"date\":\"2026-11-22T04:30:05\",\"session_key\":100,\"category\":\"SafetyCar\",\"message\":\"SAFETY CAR IN THIS LAP\"}]");
  CHECK(m->popAlert(x) && x.kind == ALERT_SC_ENDING);
  CHECK(m->popAlert(x) && x.kind == ALERT_PIT && x.a == 44);
  CHECK(m->popAlert(x) && x.kind == ALERT_PIT && x.a == 1);
  CHECK(!m->popAlert(x));
  delete m;
}

// More stints, drivers and mini-sectors than the tables hold: the extra ones
// are left out and nothing beside the tables is written.
static void testBounds() {
  F1Model *m = fresh(F1_PRACTICE);
  laps(*m, "[{\"session_key\":100,\"driver_number\":44,\"lap_number\":40,\"date_start\":\"2026-11-22T04:30:00\"}]",
       f1ParseIso("2026-11-22T05:00:00"));
  std::string st = "[";
  for (int i = 1; i <= 12; ++i) {
    char one[160];
    snprintf(one, sizeof one, "%s{\"session_key\":100,\"driver_number\":44,\"stint_number\":%d,\"lap_start\":%d,\"compound\":\"SOFT\",\"tyre_age_at_start\":0}",
             i > 1 ? "," : "", i, i * 3);
    st += one;
  }
  st += "]";
  F1Driver *d = m->driver(44);
  float guard = d->pitLane = 12.5f;     // the field after stint[]
  m->ingestStints(st.data(), st.size());
  CHECK(d->nStint == F1_MAX_STINTS);
  CHECK(d->stint[F1_MAX_STINTS - 1].lapStart == F1_MAX_STINTS * 3);
  CHECK(d->pitLane == guard);
  CHECK(d->stintLap == 36 && d->stops == 11 && d->compound == 'S');   // the set on the car is still the newest

  std::string many = "[";
  for (int i = 1; i <= 40; ++i) {
    char one[120];
    snprintf(one, sizeof one, "%s{\"driver_number\":%d,\"name_acronym\":\"D%02d\",\"team_colour\":\"808080\"}", i > 1 ? "," : "", i + 50, i);
    many += one;
  }
  many += "]";
  m->ingestDrivers(many.data(), many.size());
  CHECK(m->live.nDrv == F1_MAX_DRIVERS);

  std::string seg = "[{\"session_key\":100,\"driver_number\":44,\"lap_number\":41,\"date_start\":\"2026-11-22T04:40:00\","
                    "\"duration_sector_1\":30.0,\"duration_sector_2\":30.0,\"duration_sector_3\":30.0";
  for (int s = 1; s <= 3; ++s) {
    seg += ",\"segments_sector_" + std::to_string(s) + "\":[";
    for (int i = 0; i < 20; ++i) seg += i ? ",2049" : "2049";
    seg += "]";
  }
  seg += "}]";
  laps(*m, seg.c_str(), f1ParseIso("2026-11-22T05:00:00"));
  CHECK(d->nSeg[0] + d->nSeg[1] + d->nSeg[2] == F1_MAX_SEGS);
  delete m;
}

int main() {
  testIso();
  testGridOrder();
  testLastLap();
  testRaceControl();
  testRaceControlOrder();
  testProgress();
  testStaleAlert();
  testSectorRollback();
  testAlertOrder();
  testBounds();
  printf("%d checks, %d failed\n", g_run, g_fail);
  return g_fail ? 1 : 0;
}
