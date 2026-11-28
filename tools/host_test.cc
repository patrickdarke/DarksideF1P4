// Desktop check of f1_model against saved OpenF1 responses.
//   g++ -std=c++17 -I lib/ArduinoJson/src -I . tools/host_test.cc f1_model.cpp -o /tmp/f1test
//   tools/fetch_samples.sh && /tmp/f1test tools/samples
// Replays a whole session through the same date-windowed ingest calls the
// firmware makes, prints the tower at a few points plus every alert, and
// checks the end state (exit status 1 if a check fails). Each session is
// replayed twice: with polls on the whole second, and 1234 ms off it - the
// feed's rows are not in date order to the millisecond, and only the second
// run shows what that does.
#include "f1_model.h"
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::string slurp(const std::string &p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

// A saved full response, split into rows once so "?date>A&date<=B" can be
// emulated cheaply for every poll of the replay.
struct Feed {
  std::vector<std::pair<int64_t, std::string>> rows;
  void load(const std::string &path, const char *field) {
    std::string body = slurp(path);
    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc.is<JsonArray>()) return;
    for (JsonObject r : doc.as<JsonArray>()) {
      std::string one; serializeJson(r, one);
      rows.push_back({f1ParseIso(r[field] | ""), one});
    }
  }
  std::string window(int64_t a, int64_t b, bool keepUndated = false) const {
    std::string out = "[";
    for (auto &r : rows) {
      bool in = r.first ? (r.first > a && r.first <= b) : keepUndated;
      if (!in) continue;
      if (out.size() > 1) out += ",";
      out += r.second;
    }
    return out + "]";
  }
};

static const char *trk(F1Track t) {
  static const char *n[] = {"CLEAR", "YELLOW", "SC", "VSC", "RED", "FINISHED"};
  return n[t];
}

static void dump(F1Model &m, int64_t now, int rows) {
  char iso[32]; f1FmtIso(now, iso, sizeof iso);
  printf("-- %s lap %d %s%s started=%d finished=%d Q%d air %.1f track %.1f | %s\n", iso, m.live.lap,
         trk(m.live.track), m.live.scEnding ? " (ending)" : "", m.live.started, m.live.finished,
         m.live.qPhase, m.live.airT, m.live.trackT, m.live.note);
  for (int i = 0; i < m.live.nDrv && i < rows; ++i) {
    const F1Driver &d = m.live.drv[m.live.order[i]];
    char last[16], best[16]; bool dim;
    f1FmtLap(d.lastLap, last, sizeof last);
    f1FmtLap(m.shownBest(d, &dim), best, sizeof best);
    printf("  P%-2d %s %-16s grid %-2d int %7.3f gap %7.3f L%d/%d  %c%-2d stops %d  last %-9s best %s%s\n",
           d.pos, d.who.code, d.who.team, d.gridPos, d.interval, d.gap, d.intLaps, d.gapLaps,
           d.compound, d.tyreAge, d.stops, last, best, dim ? " (earlier phase)" : "");
  }
}

static int g_fail = 0;
#define CHECK(cond) \
  do { if (!(cond)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct Outcome {
  int drivers, lap, alerts, ovtAlerts, flagAlerts, qPhase;
  int rcRows, rcInWindow;      // race-control rows applied / dated within the replay
  F1Track track;
  F1Progress progress;
  bool started, finished;
  std::string p1;
  float fastest;
};

// key: the session's own key. The model drops rows of any other session, so
// a made-up key here means a replay that ingests nothing (as this test did,
// unnoticed, from the day that filter went in).
static Outcome replay(const std::string &dir, const char *pfx, int key, F1Kind kind, int64_t t0, int64_t t1,
                      const std::vector<int64_t> &dumps, bool print = true) {
  auto P = [&](const char *e) { return dir + "/" + pfx + "_" + e + ".json"; };
  std::string drivers = slurp(P("drivers")), stints = slurp(P("stints"));
  Feed pos, iv, laps, rc, pit, ovt, wx;
  pos.load(P("position"), "date"); iv.load(P("intervals"), "date");
  laps.load(P("laps"), "date_start"); rc.load(P("race_control"), "date");
  pit.load(P("pit"), "date"); ovt.load(P("overtakes"), "date"); wx.load(P("weather"), "date");
  F1Model *mp = new F1Model; F1Model &m = *mp;
  m.resetLive(key, kind);
  Outcome o = {};
  o.drivers = m.ingestDrivers(drivers.data(), drivers.size());
  if (print) printf("drivers: %d\n", o.drivers);
  int alerts = 0, ovtAlerts = 0; size_t di = 0;
  for (int64_t now = t0; now <= t1; now += 5000) {
    std::string s;
    // As pollFeed() asks: a little before the cursor (F1_RC_REORDER_MS).
    s = rc.window(m.curRc > F1_RC_REORDER_MS ? m.curRc - F1_RC_REORDER_MS : m.curRc, now);
    int r = m.ingestRaceControl(s.data(), s.size(), now);
    if (r > 0) o.rcRows += r;
    s = pos.window(m.curPos, now); m.ingestPosition(s.data(), s.size());
    if (kind == F1_RACE) {
      s = iv.window(m.curInt ? m.curInt : now - 30000, now); m.ingestIntervals(s.data(), s.size());
      s = pit.window(m.curPit, now);  m.ingestPit(s.data(), s.size());
      s = ovt.window(m.curOvt, now);  m.ingestOvertakes(s.data(), s.size());
    }
    s = laps.window(now - 300000, now, true); m.ingestLaps(s.data(), s.size(), now);
    m.ingestStints(stints.data(), stints.size());
    s = wx.window(m.curWx, now);   m.ingestWeather(s.data(), s.size());
    m.flushOvertakes(now);
    if (now == t0) m.alertsArmed = true;        // as the firmware does after the first load
    F1Alert a;
    while (m.popAlert(a)) {
      static const char *n[] = {"-", "START", "GREEN", "SC", "SC ENDING", "VSC", "VSC ENDING", "RED",
                                "CHEQUERED", "OVERTAKE", "PIT", "RADIO", "FASTEST"};   // F1AlertKind
      const char *name = a.kind < sizeof n / sizeof n[0] ? n[a.kind] : "?";
      char iso[32]; f1FmtIso(now, iso, sizeof iso);
      F1Driver *x = m.driver(a.a), *y = m.driver(a.b);
      if (print)
        printf("ALERT %s %s %s %s P%d (lap %d)\n", iso + 11, name, x ? x->who.code : "",
               y ? y->who.code : "", a.pos, m.live.lap);
      ++alerts;
      if (a.kind == ALERT_OVERTAKE) ++ovtAlerts;
      if (a.kind < ALERT_OVERTAKE) ++o.flagAlerts;
    }
    if (print && di < dumps.size() && now >= dumps[di]) { dump(m, now, 22); ++di; }
  }
  if (print) {
    dump(m, t1, 22);
    printf("alerts: %d (overtakes %d), race-control rows applied: %d\n", alerts, ovtAlerts, o.rcRows);
  }
  for (auto &row : rc.rows)
    if (row.first && row.first <= t0 + (t1 - t0) / 5000 * 5000) ++o.rcInWindow;
  o.lap = m.live.lap;
  o.alerts = alerts;
  o.ovtAlerts = ovtAlerts;
  o.qPhase = m.live.qPhase;
  o.track = m.live.track;
  o.progress = m.progress();
  o.started = m.live.started;
  o.finished = m.live.finished;
  o.fastest = m.live.fastest;
  if (m.live.nDrv) o.p1 = m.live.drv[m.live.order[0]].who.code;
  delete mp;
  return o;
}


int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : ".";
  char buf[40];
  f1FmtIso(f1ParseIso("2026-11-22T04:50:03.173000+00:00"), buf, sizeof buf);
  printf("iso round trip: %s\n", buf);
  if (strcmp(buf, "2026-11-22T04:50:03.173")) { puts("FAIL iso"); return 1; }
  f1FmtLap(94.151f, buf, sizeof buf); printf("lap fmt: %s\n", buf);

  F1Model *mp = new F1Model; F1Model &m = *mp;
  std::string s = slurp(dir + "/sessions2026.json");
  printf("sessions: %d\n", m.ingestSessions(s.data(), s.size()));
  s = slurp(dir + "/meetings2026.json");
  printf("meetings: %d\n", m.ingestMeetings(s.data(), s.size()));
  int64_t now = f1ParseIso("2026-11-27T15:20:00+00:00");
  const F1SessionInfo *nx = m.nextSession(now), *le = m.lastEnded(now, false), *lr = m.lastEnded(now, true);
  printf("next: %s (%s)  last ended: %s  last race: %s key %d  active now: %s\n", nx ? nx->name : "-",
         nx ? m.meetingByKey(nx->meetingKey)->name : "-", le ? le->name : "-",
         lr ? m.meetingByKey(lr->meetingKey)->name : "-", lr ? lr->key : 0,
         m.activeSession(now, 600000, 1800000) ? "yes" : "no");
  s = slurp(dir + "/race_drivers.json"); m.ingestWho(s.data(), s.size());
  s = slurp(dir + "/race_championship_drivers.json");
  printf("driver standings: %d\n", m.ingestDriverStandings(s.data(), s.size(), 11420));
  for (int i = 0; i < 3; ++i) {
    const F1Who *w = m.who(m.info.drvStand[i].num);
    printf("  %d (was %d) %s %.0f\n", m.info.drvStand[i].pos, m.info.drvStand[i].posStart,
           w ? w->code : "?", m.info.drvStand[i].pts);
  }
  s = slurp(dir + "/race_championship_teams.json");
  int nt = m.ingestTeamStandings(s.data(), s.size());
  printf("team standings: %d, leader %s %.0f\n", nt, m.info.teamStand[0].team, m.info.teamStand[0].pts);
  s = slurp(dir + "/race_session_result.json");
  printf("race result rows: %d\n", m.ingestResult(s.data(), s.size(), 11420));
  for (int i : {0, 1, 12, 20, 21}) {
    const F1ResultRow &r = m.info.result[i]; const F1Who *w = m.who(r.num);
    printf("  P%d %s gap %.3f laps %d dnf %d dns %d\n", r.pos, w ? w->code : "?", r.gap, r.laps, r.dnf, r.dns);
  }
  s = slurp(dir + "/quali_session_result.json");
  m.ingestResult(s.data(), s.size(), 11416);
  f1FmtLap(m.info.result[0].time, buf, sizeof buf);
  printf("quali P1 %s  P2 gap %.3f  P20 time %.3f\n", buf, m.info.result[1].gap, m.info.result[19].time);
  const char *nores = "{\"detail\":\"No results found.\"}";
  int e1 = m.ingestPosition(nores, strlen(nores)), e2 = m.ingestPosition("<html>", 6),
      e3 = m.ingestPosition("[{\"date\":\"2026", 16);
  printf("no-results body -> %d, html -> %d, truncated -> %d\n", e1, e2, e3);
  delete mp;

  // Las Vegas 2026: race 11420 (50 laps, won by VER, one safety car),
  // qualifying 11416.
  const int64_t r0 = f1ParseIso("2026-11-22T03:50:00+00:00"), r1 = f1ParseIso("2026-11-22T05:35:00+00:00");
  const int64_t q0 = f1ParseIso("2026-11-21T03:55:00+00:00"), q1 = f1ParseIso("2026-11-21T05:05:00+00:00");
  puts("\n===== RACE replay =====");
  Outcome race = replay(dir, "race", 11420, F1_RACE, r0, r1,
                        {f1ParseIso("2026-11-22T04:00:00+00:00"), f1ParseIso("2026-11-22T04:40:00+00:00")});
  puts("\n===== QUALI replay =====");
  Outcome quali = replay(dir, "quali", 11416, F1_QUALI, q0, q1, {f1ParseIso("2026-11-21T04:35:00+00:00")});

  CHECK(race.drivers == 22);
  CHECK(race.lap == 50);
  CHECK(race.p1 == "VER");
  CHECK(race.started && race.finished && race.track == TRK_FINISHED && race.progress == PROG_OVER);
  CHECK(race.flagAlerts >= 3);                  // lights out, safety car, chequered flag at the least
  CHECK(race.fastest > 60 && race.fastest < 120);
  CHECK(race.rcRows == race.rcInWindow && race.rcRows > 90);   // every row, once
  CHECK(quali.drivers == 22);
  CHECK(quali.qPhase == 3);
  CHECK(quali.started && quali.finished && quali.track == TRK_FINISHED && quali.progress == PROG_OVER);

  // Part-way: the race under way, qualifying in the break between Q1 and Q2
  // (where the feed already tags rows as Q2).
  CHECK(replay(dir, "race", 11420, F1_RACE, r0, f1ParseIso("2026-11-22T04:40:00+00:00"), {}, false).progress == PROG_UNDER_WAY);
  CHECK(replay(dir, "quali", 11416, F1_QUALI, q0, f1ParseIso("2026-11-21T04:25:00+00:00"), {}, false).progress == PROG_BREAK);
  CHECK(replay(dir, "quali", 11416, F1_QUALI, q0, f1ParseIso("2026-11-21T04:46:00+00:00"), {}, false).progress == PROG_BREAK);
  CHECK(replay(dir, "quali", 11416, F1_QUALI, q0, f1ParseIso("2026-11-21T04:55:00+00:00"), {}, false).progress == PROG_UNDER_WAY);
  CHECK(quali.rcRows == quali.rcInWindow && quali.rcRows > 60);

  // The same sessions with the polls 1234 ms off the whole second.
  Outcome race2 = replay(dir, "race", 11420, F1_RACE, r0 + 1234, r1 + 1234, {}, false);
  Outcome quali2 = replay(dir, "quali", 11416, F1_QUALI, q0 + 1234, q1 + 1234, {}, false);
  CHECK(race2.rcRows == race2.rcInWindow && race2.track == race.track && race2.finished == race.finished);
  CHECK(race2.lap == race.lap && race2.p1 == race.p1 && race2.flagAlerts == race.flagAlerts);
  CHECK(quali2.rcRows == quali2.rcInWindow && quali2.track == quali.track && quali2.finished == quali.finished);
  CHECK(quali2.qPhase == quali.qPhase);

  printf("\nhost_test: %d check(s) failed\n", g_fail);
  return g_fail ? 1 : 0;
}
