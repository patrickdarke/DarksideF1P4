// Desktop check of f1_model against saved OpenF1 responses.
//   g++ -std=c++17 -I lib/ArduinoJson/src -I . tools/host_test.cc f1_model.cpp -o /tmp/f1test
//   tools/fetch_samples.sh && /tmp/f1test tools/samples
// Replays a whole session through the same date-windowed ingest calls the
// firmware makes and prints the tower at a few points plus every alert.
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

static int replay(const std::string &dir, const char *pfx, F1Kind kind, int64_t t0, int64_t t1,
                  const std::vector<int64_t> &dumps) {
  auto P = [&](const char *e) { return dir + "/" + pfx + "_" + e + ".json"; };
  std::string drivers = slurp(P("drivers")), stints = slurp(P("stints"));
  Feed pos, iv, laps, rc, pit, ovt, wx;
  pos.load(P("position"), "date"); iv.load(P("intervals"), "date");
  laps.load(P("laps"), "date_start"); rc.load(P("race_control"), "date");
  pit.load(P("pit"), "date"); ovt.load(P("overtakes"), "date"); wx.load(P("weather"), "date");
  F1Model *mp = new F1Model; F1Model &m = *mp;
  m.resetLive(1, kind);
  printf("drivers: %d\n", m.ingestDrivers(drivers.data(), drivers.size()));
  int alerts = 0, ovtAlerts = 0; size_t di = 0;
  for (int64_t now = t0; now <= t1; now += 5000) {
    std::string s;
    s = rc.window(m.curRc, now);   m.ingestRaceControl(s.data(), s.size());
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
      static const char *n[] = {"-", "START", "GREEN", "SC", "SC ENDING", "VSC", "VSC ENDING",
                                "RED", "CHEQUERED", "OVERTAKE"};
      char iso[32]; f1FmtIso(now, iso, sizeof iso);
      F1Driver *x = m.driver(a.a), *y = m.driver(a.b);
      printf("ALERT %s %s %s %s P%d (lap %d)\n", iso + 11, n[a.kind], x ? x->who.code : "",
             y ? y->who.code : "", a.pos, m.live.lap);
      ++alerts; if (a.kind == ALERT_OVERTAKE) ++ovtAlerts;
    }
    if (di < dumps.size() && now >= dumps[di]) { dump(m, now, 22); ++di; }
  }
  dump(m, t1, 22);
  printf("alerts: %d (overtakes %d)\n", alerts, ovtAlerts);
  delete mp;
  return 0;
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

  puts("\n===== RACE replay =====");
  replay(dir, "race", F1_RACE, f1ParseIso("2026-11-22T03:50:00+00:00"), f1ParseIso("2026-11-22T05:35:00+00:00"),
         {f1ParseIso("2026-11-22T04:00:00+00:00"), f1ParseIso("2026-11-22T04:40:00+00:00")});
  puts("\n===== QUALI replay =====");
  replay(dir, "quali", F1_QUALI, f1ParseIso("2026-11-21T03:55:00+00:00"), f1ParseIso("2026-11-21T05:05:00+00:00"),
         {f1ParseIso("2026-11-21T04:35:00+00:00")});
  return 0;
}
