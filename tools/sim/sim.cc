// Host render of ui.cpp: LVGL draws into a framebuffer that is dumped as PPM.
#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "f1_app.h"
#include "config.h"
#include "settings.h"
#include "ui.h"

static uint32_t g_ms = 1000;
uint32_t millis() { return g_ms; }
static F1Model *g_m; static AppStatus g_st; static int64_t g_wall;
bool appLock(uint32_t) { return true; } void appUnlock() {}
F1Model &appModel() { return *g_m; } const AppStatus &appStatus() { return g_st; }
int64_t appWallMs() { return g_wall; }
void appWantFast(bool, uint8_t) {}
const uint8_t *headGet(uint8_t, uint16_t *, uint16_t *) { return nullptr; }
void soundRaceStart() {}
const Settings &settings() { static Settings s = {"", "", "", "", "", true, true}; return s; }

static lv_color_t fb[1024 * 600];
static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
  for (int y = a->y1; y <= a->y2; ++y) for (int x = a->x1; x <= a->x2; ++x) fb[y * 1024 + x] = *px++;
  lv_disp_flush_ready(d);
}
static void run(int ms) { for (int i = 0; i < ms; i += 10) { g_ms += 10; g_wall += 10; lv_tick_inc(10); lv_timer_handler(); } }
static void shot(const char *name) {
  lv_obj_invalidate(lv_scr_act()); run(50);
  std::ofstream f(std::string("out/") + name + ".ppm", std::ios::binary);
  f << "P6\n1024 600\n255\n";
  for (auto &c : fb) { uint32_t v = lv_color_to32(c); char p[3] = {(char)(v >> 16), (char)(v >> 8), (char)v}; f.write(p, 3); }
}
static std::string slurp(const std::string &p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
struct Feed {
  std::vector<std::pair<int64_t, std::string>> rows;
  void load(const std::string &path, const char *field) {
    std::string body = slurp(path); JsonDocument doc;
    if (deserializeJson(doc, body) || !doc.is<JsonArray>()) return;
    for (JsonObject r : doc.as<JsonArray>()) { std::string one; serializeJson(r, one); rows.push_back({f1ParseIso(r[field] | ""), one}); }
  }
  std::string window(int64_t a, int64_t b, bool undated = false) const {
    std::string out = "[";
    for (auto &r : rows) { bool in = r.first ? (r.first > a && r.first <= b) : undated; if (!in) continue; if (out.size() > 1) out += ","; out += r.second; }
    return out + "]";
  }
};
static std::string D = "samples/";   // run from tools/ after ./fetch_samples.sh
static void advance(const char *pfx, F1Kind kind, int key, const char *from, const char *to, bool fresh) {
  static Feed pos, iv, laps, rc, pit, ovt, wx; static std::string stints, cur;
  F1Model &m = *g_m;
  if (fresh || cur != pfx) {
    cur = pfx; pos = iv = laps = rc = pit = ovt = wx = Feed();
    auto P = [&](const char *e) { return D + pfx + "_" + e + ".json"; };
    pos.load(P("position"), "date"); iv.load(P("intervals"), "date"); laps.load(P("laps"), "date_start");
    rc.load(P("race_control"), "date"); pit.load(P("pit"), "date"); ovt.load(P("overtakes"), "date"); wx.load(P("weather"), "date");
    stints = slurp(P("stints"));
    m.resetLive(key, kind);
    std::string d = slurp(P("drivers")); m.ingestDrivers(d.data(), d.size());
  }
  int64_t t0 = f1ParseIso(from), t1 = f1ParseIso(to);
  for (int64_t now = t0; now <= t1; now += 5000) {
    std::string s;
    s = rc.window(m.curRc, now); m.ingestRaceControl(s.data(), s.size());
    s = pos.window(m.curPos, now); m.ingestPosition(s.data(), s.size());
    if (kind == F1_RACE) {
      s = iv.window(m.curInt ? m.curInt : now - 30000, now); m.ingestIntervals(s.data(), s.size());
      s = pit.window(m.curPit, now); m.ingestPit(s.data(), s.size());
      s = ovt.window(m.curOvt, now); m.ingestOvertakes(s.data(), s.size());
    }
    s = laps.window(now - 300000, now, true); m.ingestLaps(s.data(), s.size(), now);
    m.ingestStints(stints.data(), stints.size());
    s = wx.window(m.curWx, now); m.ingestWeather(s.data(), s.size());
    m.flushOvertakes(now);
    if (now == t0) m.alertsArmed = true;
  }
}
int main() {
  setenv("TZ", LOCAL_TZ, 1); tzset();
  lv_init();
  static lv_disp_draw_buf_t db; static lv_color_t buf[1024 * 100];
  lv_disp_draw_buf_init(&db, buf, nullptr, 1024 * 100);
  static lv_disp_drv_t drv; lv_disp_drv_init(&drv);
  drv.hor_res = 1024; drv.ver_res = 600; drv.flush_cb = flush; drv.draw_buf = &db; lv_disp_drv_register(&drv);
  g_m = new F1Model; F1Model &m = *g_m; memset(&g_st, 0, sizeof g_st);
  g_wall = f1ParseIso("2026-11-27T15:40:00+00:00");
  uiBegin();
  strcpy(g_st.msg, "Connecting to Wi-Fi darkside"); g_st.mode = APP_BOOT; run(600); shot("0_boot");

  std::string s = slurp(D + "sessions2026.json"); m.ingestSessions(s.data(), s.size());
  s = slurp(D + "meetings2026.json"); m.ingestMeetings(s.data(), s.size());
  strcpy(g_st.msg, "192.168.1.57"); g_st.mode = APP_IDLE; run(600); shot("1_next");
  { std::string t = slurp(D + "fake_location.json"); const F1SessionInfo *nx = m.nextSession(g_wall);
    printf("track points: %d\n", m.ingestTrack(t.data(), t.size(), nx->circuitKey)); run(600); shot("1b_next_map"); }
  s = slurp(D + "race_drivers.json"); m.ingestWho(s.data(), s.size());
  s = slurp(D + "race_championship_drivers.json"); m.ingestDriverStandings(s.data(), s.size(), 11420);
  s = slurp(D + "race_championship_teams.json"); m.ingestTeamStandings(s.data(), s.size());
  s = slurp(D + "race_session_result.json"); m.ingestResult(s.data(), s.size(), 11420);
  run(40000); shot("1c_no_rotation");          // nothing changes screens on its own (IDLE_ROTATE_S 0)
  uiDebug("I2"); run(600); shot("2_result"); uiDebug("I3"); run(600); shot("3_drivers");
  uiDebug("I4"); run(600); shot("4_teams");
  s = slurp(D + "quali_session_result.json"); m.ingestResult(s.data(), s.size(), 11416);
  uiDebug("I2"); run(600); shot("5_quali_result");

  g_st.mode = APP_LIVE; g_st.replay = false;
  advance("race", F1_RACE, 11420, "2026-11-22T03:50:00+00:00", "2026-11-22T04:01:00+00:00", true);
  run(600); shot("6_race_grid");
  advance("race", F1_RACE, 11420, "2026-11-22T04:01:05+00:00", "2026-11-22T04:03:50+00:00", false);
  run(600); shot("7_race_lights_out");
  advance("race", F1_RACE, 11420, "2026-11-22T04:03:55+00:00", "2026-11-22T04:26:00+00:00", false);
  { F1Alert a; while (m.popAlert(a)) {} } run(8000); shot("8_race_green");
  advance("race", F1_RACE, 11420, "2026-11-22T04:26:05+00:00", "2026-11-22T04:28:00+00:00", false);
  run(600); shot("9_race_overtake_strip");
  { F1Alert a; while (m.popAlert(a)) {} } run(5000);
  advance("race", F1_RACE, 11420, "2026-11-22T04:28:05+00:00", "2026-11-22T04:34:30+00:00", false);
  { F1Alert a, last = {}; while (m.popAlert(a)) last = a; } 
  advance("race", F1_RACE, 11420, "2026-11-22T04:34:35+00:00", "2026-11-22T04:36:30+00:00", false);
  run(8000); shot("10_race_sc_tower");
  uiDebug("V3"); run(600); shot("10b_tyres"); uiDebug("V0"); run(600);
  advance("race", F1_RACE, 11420, "2026-11-22T04:36:35+00:00", "2026-11-22T05:31:20+00:00", false);
  { F1Alert a; while (m.popAlert(a)) {} } run(8000);
  advance("race", F1_RACE, 11420, "2026-11-22T05:31:25+00:00", "2026-11-22T05:31:30+00:00", false);
  run(600); shot("11_race_chequered");
  run(8000); { F1Alert a; while (m.popAlert(a)) {} } run(4000); shot("12_race_final");
  uiDebug("V3"); run(600); shot("12b_tyres_final"); uiDebug("V0"); run(600);

  advance("quali", F1_QUALI, 11416, "2026-11-21T03:55:00+00:00", "2026-11-21T04:35:00+00:00", true);
  run(600); shot("13_quali_q2");
  return 0;
}
