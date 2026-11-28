#include "ui.h"

#include <Arduino.h>
#include <lvgl.h>
#include <time.h>
#include <initializer_list>

#include "config.h"
#include "f1_app.h"
#include "f1_head.h"
#include "settings.h"
#include "sound.h"

// ---- theme ---------------------------------------------------------------
#define C_BG     0x0B0D10
#define C_PANEL  0x161A20
#define C_ROW    0x12151A
#define C_TEXT   0xF2F4F7
#define C_DIM    0x8A93A0
#define C_RED    0xE10600
#define C_GREEN  0x21C55D
#define C_YELLOW 0xFFD400
#define C_UP     0x3DDC84
#define C_DOWN   0xFF5A5A
#define C_PURPLE 0xC06BFF
#define C_OFF    0x2A303A

#define F14 &lv_font_montserrat_14
#define F16 &lv_font_montserrat_16
#define F20 &lv_font_montserrat_20
#define F24 &lv_font_montserrat_24
#define F28 &lv_font_montserrat_28
#define F32 &lv_font_montserrat_32
#define F40 &lv_font_montserrat_40
#define F48 &lv_font_montserrat_48

#define SCR_W 1024
#define SCR_H 600
#define ROWS  22
#define PER_COL 11
#define ROW_H 44
#define ROW_Y0 70
#define COL_W 500

struct Row {
  lv_obj_t *box, *pos, *bar, *name, *delta, *ring, *tyre, *age, *c1, *c2;
};

static lv_obj_t *s_table, *s_next, *s_boot;
static lv_obj_t *s_title, *s_center, *s_pill, *s_pillLbl, *s_sub, *s_note, *s_clock;
static Row s_rows[ROWS];
static lv_obj_t *s_nxKicker, *s_nxMeeting, *s_nxSession, *s_nxCount, *s_nxTimes, *s_nxNames, *s_nxStatus;
static lv_obj_t *s_map, *s_mapLine, *s_mapDot, *s_mapName;
static lv_point_t s_mapPts[F1_MAX_TRACK + 1];
static uint32_t s_mapVersion = 0;
static int s_mapCircuit = -1;
static lv_obj_t *s_alert, *s_alertBig, *s_alertSub, *s_alertSub2;
static lv_obj_t *s_toast, *s_toastBar, *s_toastLbl;
static uint32_t s_alertUntil = 0, s_toastUntil = 0;

enum IdleView { V_NEXT, V_GRID, V_RESULT, V_DRIVERS, V_TEAMS, V_COUNT };
static int s_view = V_NEXT;
static uint32_t s_viewSince = 0;
static bool s_skip = false;

// Live screens: picked with the footer tabs (a header tap steps to the next
// one); a tap on a driver's row opens that car.
enum LiveView { LV_TOWER, LV_MAP, LV_SECTORS, LV_TYRES, LV_DRIVER, LV_COUNT };
static int s_liveView = LIVE_VIEW_START % LV_COUNT;
static uint8_t s_focus = 0;                 // car on the driver screen
static bool s_isLive = false;
static uint8_t s_orderNum[F1_MAX_DRIVERS];  // running order as last drawn (for taps)
static int s_orderN = 0;
static volatile int s_cmdView = -1, s_cmdDriver = -1, s_cmdIdle = -1, s_cmdAlert = -1;   // from the serial console
static int s_backView = LV_TOWER;           // screen the car screen was opened from
static lv_obj_t *s_tabs[LV_COUNT], *s_tabLbl[LV_COUNT], *s_streamDot;
#define TAB_X 532                           // the five screen tabs in the footer
#define TAB_W 78

// Map screen
#define LMAP_X 316
#define LMAP_W 696
#define LMAP_H 436
struct MapRow { lv_obj_t *pos, *bar, *code, *tyre, *gap; };
static lv_obj_t *s_pMap, *s_lmap, *s_lmapSeg[3], *s_lmapKey[3], *s_lmapSF, *s_lmapNone, *s_wx;
// Sector colours on the circuit: a little muted so the cars stand out.
static const uint32_t SECTOR_COLOUR[3] = {0xC43A44, 0x2E7BD6, 0xD4B11E};
#define C_TRACK 0x4A5362
static MapRow s_mrow[ROWS];
static lv_obj_t *s_dot[F1_MAX_DRIVERS], *s_dotLbl[F1_MAX_DRIVERS];
// The outline twice over, so a stretch that runs across the start/finish
// line is still one run of points.
static lv_point_t s_lmapPts[2 * F1_MAX_TRACK + 1];
static lv_obj_t *s_lmapFlag[F1_MAX_MSEC], *s_lmapFlagKey, *s_lmapCorner[F1_MAX_CORNERS];
static int s_lmExtras = -1;              // corners and marshal sectors the map was laid out with
#define C_FLAG 0xFFE600                  // a marshal sector under yellow: brighter and wider than sector 3
#define CORNER_OFF 17                    // px from the track to a corner number
static float s_lmK = 0;
static int s_lmOX = 0, s_lmOY = 0, s_lmCircuit = -1, s_lmN = 0, s_lmSec2 = -1;
static uint32_t s_fastSeen = 0;

// Sector screen
struct SecRow { lv_obj_t *box, *pos, *bar, *name, *s[3], *lap, *trap, *strip; };
static lv_obj_t *s_pSec;
static SecRow s_srow[ROWS];
static uint8_t s_segCopy[ROWS][F1_MAX_SEGS], s_segN[ROWS][3];

// Driver screen
static lv_obj_t *s_pDrv, *s_dBar, *s_dPos, *s_dCode, *s_dName, *s_dTeam, *s_dGap, *s_dTyre, *s_dPit;
static lv_obj_t *s_dSpeed, *s_dGear, *s_dRpm, *s_dRpmBar, *s_dThr, *s_dBrk;
static lv_obj_t *s_dSec[3], *s_dSecBest[3], *s_dLast, *s_dBest, *s_dTrap;
static lv_obj_t *s_dImg;
static lv_img_dsc_t s_headDsc;
static const uint8_t *s_headPx = (const uint8_t *)1;   // what s_dImg shows (1 = nothing set yet)

// ---- helpers ---------------------------------------------------------------

static lv_obj_t *mkBox(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, int radius = 0) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
  return o;
}

static lv_obj_t *mkLabel(lv_obj_t *parent, int x, int y, int w, const lv_font_t *font,
                         uint32_t colour, lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_pos(l, x, y);
  if (w > 0) {
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  }
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
  lv_obj_set_style_text_align(l, align, 0);
  lv_label_set_text(l, "");
  return l;
}

static void setText(lv_obj_t *l, const char *t) {
  if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}
// Setting a style or a flag redraws the object even when nothing changes,
// so all of these look first.
static void setColour(lv_obj_t *l, uint32_t c) {
  lv_color_t n = lv_color_hex(c);
  if (lv_obj_get_style_text_color(l, 0).full != n.full) lv_obj_set_style_text_color(l, n, 0);
}
static void setBg(lv_obj_t *o, uint32_t c) {
  lv_color_t n = lv_color_hex(c);
  if (lv_obj_get_style_bg_color(o, 0).full != n.full) lv_obj_set_style_bg_color(o, n, 0);
}
static void setFont(lv_obj_t *l, const lv_font_t *f) {
  if (lv_obj_get_style_text_font(l, 0) != f) lv_obj_set_style_text_font(l, f, 0);
}
static void show(lv_obj_t *o, bool on) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != on) return;
  if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void upper(char *s) {
  for (; *s; ++s) *s = (char)toupper((unsigned char)*s);
}

static uint32_t tyreColour(char c) {
  switch (c) {
    case 'S': return 0xFF3333;
    case 'M': return 0xFFD400;
    case 'H': return 0xF2F4F7;
    case 'I': return 0x3DDC84;
    case 'W': return 0x3B82F6;
    default:  return C_DIM;
  }
}

// The feed is in Celsius, with car and speed-trap speeds in km/h and the wind
// in m/s. The panel's settings say what to show.
static bool inF() { return settings().fahrenheit; }
static bool inMph() { return settings().mph; }
static float temp(float c) { return inF() ? c * 9.0f / 5.0f + 32.0f : c; }
static const char *tempUnit() { return inF() ? "°F" : "°C"; }
static int speed(int kmh) { return inMph() ? (kmh * 1000 + 804) / 1609 : kmh; }
static const char *speedUnit() { return inMph() ? "mph" : "km/h"; }
static void fmtWind(float ms, char *out, size_t n) {
  if (inMph()) snprintf(out, n, "%.0f mph", ms * 2.23694f);
  else snprintf(out, n, "%.1f m/s", ms);
}

static void fmtGap(float sec, uint8_t laps, char *out, size_t n) {
  if (laps) snprintf(out, n, "+%d LAP%s", laps, laps > 1 ? "S" : "");
  else if (sec < 0) out[0] = 0;
  else if (sec >= 100) snprintf(out, n, "+%.1f", sec);
  else snprintf(out, n, "+%.3f", sec);
}

// ---- table (live tower, standings, results share it) ----------------------

struct RowData {
  char pos[4];
  uint32_t colour;
  char name[26];
  bool wide;            // long name, no delta / tyre
  int delta;            // places gained (+) or lost (-); 0 hides it
  char tyre;            // 0 hides it
  int age;
  char c1[16], c2[16];
  bool dim;
};

static void setRow(int i, int n, const RowData *d) {
  Row &r = s_rows[i];
  if (!d || i >= n) { show(r.box, false); return; }
  int perCol = n <= PER_COL ? (n + 1) / 2 : PER_COL;
  if (perCol < 1) perCol = 1;
  lv_obj_set_pos(r.box, 12 + (i / perCol) * (COL_W + 4), ROW_Y0 + (i % perCol) * ROW_H);
  show(r.box, true);
  setText(r.pos, d->pos);
  setBg(r.bar, d->colour);
  setText(r.name, d->name);
  lv_obj_set_width(r.name, d->wide ? 250 : 84);
  setFont(r.name, d->wide ? F24 : F28);
  lv_obj_set_y(r.name, d->wide ? 8 : 5);
  uint32_t fg = d->dim ? C_DIM : C_TEXT;
  setColour(r.name, fg);
  setColour(r.c1, fg);
  char b[12];
  if (d->delta && !d->wide) {
    snprintf(b, sizeof b, "%s%d", d->delta > 0 ? LV_SYMBOL_UP : LV_SYMBOL_DOWN, abs(d->delta));
    setText(r.delta, b);
    setColour(r.delta, d->delta > 0 ? C_UP : C_DOWN);
  } else {
    setText(r.delta, "");
  }
  bool ty = d->tyre && !d->wide;
  show(r.ring, ty);
  show(r.age, ty);
  if (ty) {
    uint32_t tc = tyreColour(d->tyre);
    if (lv_obj_get_style_border_color(r.ring, 0).full != lv_color_hex(tc).full)
      lv_obj_set_style_border_color(r.ring, lv_color_hex(tc), 0);
    b[0] = d->tyre; b[1] = 0;
    setText(r.tyre, b);
    setColour(r.tyre, tc);
    if (d->age > 0) snprintf(b, sizeof b, "%d", d->age); else b[0] = 0;
    setText(r.age, b);
  }
  setText(r.c1, d->c1);
  setText(r.c2, d->c2);
}

static void setHeader(const char *title, const char *center, const char *pill, uint32_t pillBg,
                      uint32_t pillFg, const char *sub) {
  setText(s_title, title);
  lv_obj_set_width(s_title, (center && *center) ? 560 : 980);   // no lap counter: use the full bar
  setText(s_center, center);
  show(s_pill, pill && *pill);
  if (pill && *pill) {
    setBg(s_pill, pillBg);
    setText(s_pillLbl, pill);
    setColour(s_pillLbl, pillFg);
  }
  setText(s_sub, sub ? sub : "");
}

static void sessionTitle(const F1Model &m, int key, const char *suffix, char *out, size_t n) {
  const F1SessionInfo *s = m.sessionByKey(key);
  const F1MeetingInfo *mt = s ? m.meetingByKey(s->meetingKey) : nullptr;
  char name[44];
  strlcpy(name, mt ? mt->name : "", sizeof name);
  char *gp = strstr(name, " Grand Prix");
  if (gp) strcpy(gp, " GP");
  snprintf(out, n, "%s%s%s%s", name, name[0] ? "  |  " : "", s ? s->name : "", suffix);
  upper(out);
}

static void liveHeader(const F1Model &m, const AppStatus &st) {
  const F1Live &L = m.live;
  char title[80], center[24], sub[64];
  sessionTitle(m, L.sessionKey, st.replay ? "  (replay)" : "", title, sizeof title);
  if (L.kind == F1_RACE) {
    if (!L.started) strcpy(center, "GRID");
    else snprintf(center, sizeof center, "LAP %d", L.lap ? L.lap : 1);
  } else if (L.kind == F1_QUALI) {
    if (L.qPhase) snprintf(center, sizeof center, "Q%d", L.qPhase); else strcpy(center, "QUALI");
  } else {
    center[0] = 0;
  }
  const char *pill = "GREEN";
  uint32_t bg = C_GREEN, fg = 0x000000;
  switch (L.track) {
    case TRK_YELLOW:   pill = "YELLOW"; bg = C_YELLOW; break;
    case TRK_SC:       pill = L.scEnding ? "SC IN THIS LAP" : "SAFETY CAR"; bg = C_YELLOW; break;
    case TRK_VSC:      pill = L.scEnding ? "VSC ENDING" : "VIRTUAL SC"; bg = C_YELLOW; break;
    case TRK_RED:      pill = "RED FLAG"; bg = C_RED; fg = 0xFFFFFF; break;
    case TRK_FINISHED: pill = "CHEQUERED"; bg = 0xF2F4F7; break;
    default:
      if (!L.started) { pill = "NOT STARTED"; bg = 0x2A303A; fg = C_TEXT; }
      break;
  }
  if (L.trackT > -50)
    snprintf(sub, sizeof sub, "Track %.0f%s  Air %.0f%s%s", temp(L.trackT), tempUnit(), temp(L.airT), tempUnit(),
             L.rain ? "  RAIN" : "");
  else sub[0] = 0;
  setHeader(title, center, pill, bg, fg, sub);
}

static void renderLive(const F1Model &m) {
  const F1Live &L = m.live;
  float ref = 0;                      // P1's time in practice / qualifying
  if (L.kind != F1_RACE && L.nDrv) {
    bool dim;
    ref = m.shownBest(L.drv[L.order[0]], &dim);
    if (dim) ref = 0;
  }
  for (int i = 0; i < ROWS; ++i) {
    if (i >= L.nDrv) { setRow(i, L.nDrv, nullptr); continue; }
    const F1Driver &d = L.drv[L.order[i]];
    RowData r = {};
    if (d.pos) snprintf(r.pos, sizeof r.pos, "%d", d.pos); else strcpy(r.pos, "-");
    r.colour = d.who.colour;
    strlcpy(r.name, d.who.code, sizeof r.name);
    r.tyre = d.compound == '?' ? 0 : d.compound;
    r.age = d.tyreAge;
    if (L.kind == F1_RACE) {
      if (L.started && d.gridPos && d.pos) r.delta = (int)d.gridPos - (int)d.pos;
      bool stale = L.started && d.intMs && L.newestIntMs - d.intMs > 180000 && L.track != TRK_RED &&
                   !L.finished;
      if (stale || (L.started && L.lap > 2 && !d.intMs && d.pos > 1)) {
        strcpy(r.c1, "STOPPED");
        r.dim = true;
        r.tyre = 0;
        r.delta = 0;
      } else if (d.pos == 1 && L.started) {
        strcpy(r.c1, "LEADER");
        f1FmtLap(d.lastLap, r.c2, sizeof r.c2);
      } else {
        fmtGap(d.interval, d.intLaps, r.c1, sizeof r.c1);
        fmtGap(d.gap, d.gapLaps, r.c2, sizeof r.c2);
      }
    } else {
      bool dim;
      float t = m.shownBest(d, &dim);
      f1FmtLap(t, r.c1, sizeof r.c1);
      r.dim = dim || !(t > 0);
      if (!(t > 0)) strcpy(r.c1, "NO TIME");
      else if (!dim && ref > 0 && d.pos > 1 && t >= ref) snprintf(r.c2, sizeof r.c2, "+%.3f", t - ref);
    }
    setRow(i, L.nDrv, &r);
  }
  setText(s_note, L.note);
}

static void deltaPts(float now, float before, char *out, size_t n) {
  float d = now - before;
  if (d > 0.01f) snprintf(out, n, "+%g", d); else out[0] = 0;
}

static void renderDrivers(const F1Model &m) {
  const F1Info &I = m.info;
  char title[80];
  sessionTitle(m, I.standKey, "", title, sizeof title);
  char *bar = strstr(title, "  |  ");
  if (bar) *bar = 0;
  char t2[100];
  snprintf(t2, sizeof t2, "DRIVERS' CHAMPIONSHIP  |  AFTER %s", title);
  setHeader(t2, "", "", 0, 0, "");
  int n = I.nDrvStand > ROWS ? ROWS : I.nDrvStand;
  for (int i = 0; i < ROWS; ++i) {
    if (i >= n) { setRow(i, n, nullptr); continue; }
    const F1StandRow &s = I.drvStand[i];
    const F1Who *w = m.who(s.num);
    RowData r = {};
    snprintf(r.pos, sizeof r.pos, "%d", s.pos);
    r.colour = w ? w->colour : 0x808080;
    if (w) strlcpy(r.name, w->code, sizeof r.name); else snprintf(r.name, sizeof r.name, "#%d", s.num);
    if (s.posStart && s.pos) r.delta = (int)s.posStart - (int)s.pos;
    snprintf(r.c1, sizeof r.c1, "%g", s.pts);
    deltaPts(s.pts, s.ptsStart, r.c2, sizeof r.c2);
    setRow(i, n, &r);
  }
  setText(s_note, "Points; right column is what the last race added");
}

static void renderTeams(const F1Model &m) {
  const F1Info &I = m.info;
  setHeader("CONSTRUCTORS' CHAMPIONSHIP", "", "", 0, 0, "");
  int n = I.nTeamStand;
  for (int i = 0; i < ROWS; ++i) {
    if (i >= n) { setRow(i, n, nullptr); continue; }
    const F1StandRow &s = I.teamStand[i];
    RowData r = {};
    snprintf(r.pos, sizeof r.pos, "%d", s.pos);
    r.colour = 0x808080;
    for (int k = 0; k < I.nWho; ++k)
      if (!strcmp(I.who[k].team, s.team)) { r.colour = I.who[k].colour; break; }
    strlcpy(r.name, s.team, sizeof r.name);
    r.wide = true;
    snprintf(r.c1, sizeof r.c1, "%g", s.pts);
    deltaPts(s.pts, s.ptsStart, r.c2, sizeof r.c2);
    setRow(i, n, &r);
  }
  setText(s_note, "Points; right column is what the last race added");
}

static void renderResult(const F1Model &m) {
  const F1Info &I = m.info;
  char title[90];
  sessionTitle(m, I.resultKey, "  |  RESULT", title, sizeof title);
  setHeader(title, "", "", 0, 0, "");
  const F1SessionInfo *s = m.sessionByKey(I.resultKey);
  bool race = s && s->kind == F1_RACE;
  int n = I.nResult > ROWS ? ROWS : I.nResult;
  for (int i = 0; i < ROWS; ++i) {
    if (i >= n) { setRow(i, n, nullptr); continue; }
    const F1ResultRow &x = I.result[i];
    const F1Who *w = m.who(x.num);
    RowData r = {};
    if (x.pos) snprintf(r.pos, sizeof r.pos, "%d", x.pos); else strcpy(r.pos, "-");
    r.colour = w ? w->colour : 0x808080;
    if (w) strlcpy(r.name, w->code, sizeof r.name); else snprintf(r.name, sizeof r.name, "#%d", x.num);
    if (x.dsq) strcpy(r.c1, "DSQ");
    else if (x.dns) strcpy(r.c1, "DNS");
    else if (x.dnf) strcpy(r.c1, "DNF");
    else if (x.pos == 1 || !race) {
      if (race && x.time >= 3600) {
        int ms = (int)lroundf((x.time - floorf(x.time)) * 1000);
        int t = (int)x.time;
        if (ms >= 1000) { ms -= 1000; ++t; }
        snprintf(r.c1, sizeof r.c1, "%d:%02d:%02d.%03d", t / 3600, (t / 60) % 60, t % 60, ms);
      } else {
        f1FmtLap(x.time, r.c1, sizeof r.c1);
      }
      if (!race && x.pos > 1) fmtGap(x.gap, x.gapLaps, r.c2, sizeof r.c2);
      if (!r.c1[0]) strcpy(r.c1, "NO TIME");
    } else {
      fmtGap(x.gap, x.gapLaps, r.c1, sizeof r.c1);
    }
    r.dim = x.dnf || x.dns || x.dsq || !x.pos;
    setRow(i, n, &r);
  }
  setText(s_note, "");
}

static void renderGrid(const F1Model &m) {
  const F1Info &I = m.info;
  const F1MeetingInfo *mt = m.meetingByKey(I.gridMeeting);
  char name[44], title[80];
  strlcpy(name, mt ? mt->name : "", sizeof name);
  char *gp = strstr(name, " Grand Prix");
  if (gp) strcpy(gp, " GP");
  snprintf(title, sizeof title, "%s%sSTARTING GRID", name, name[0] ? "  |  " : "");
  upper(title);
  setHeader(title, "", "", 0, 0, "");
  int n = I.nGrid > ROWS ? ROWS : I.nGrid;
  float pole = n ? I.grid[0].lap : 0;
  for (int i = 0; i < ROWS; ++i) {
    if (i >= n) { setRow(i, n, nullptr); continue; }
    const F1GridRow &g = I.grid[i];
    const F1Who *w = m.who(g.num);
    RowData r = {};
    snprintf(r.pos, sizeof r.pos, "%d", g.pos);
    r.colour = w ? w->colour : 0x808080;
    if (w) strlcpy(r.name, w->code, sizeof r.name); else snprintf(r.name, sizeof r.name, "#%d", g.num);
    f1FmtLap(g.lap, r.c1, sizeof r.c1);
    if (i && g.lap > 0 && pole > 0 && g.lap >= pole) snprintf(r.c2, sizeof r.c2, "+%.3f", g.lap - pole);
    setRow(i, n, &r);
  }
  setText(s_note, "Qualifying time; right column is the gap to pole");
}

// ---- live: map ---------------------------------------------------------------

static const char *compass(int deg) {
  static const char *const P[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return P[((deg % 360 + 360) % 360 * 2 + 45) / 90 % 8];
}

// Car dots: feed coordinates -> outline units -> pixels in the map box.
static void placeDots(const F1Model &m, const AppStatus &st) {
  const F1Live &L = m.live;
  const F1Info &I = m.info;
  for (int i = 0; i < F1_MAX_DRIVERS; ++i) {
    bool on = false;
    if (i < L.nDrv && s_lmK > 0 && I.trackSpan > 0) {
      const F1Driver &d = L.drv[i];
      if (d.locMs && st.dataNowMs - d.locMs < 20000) {
        float X = (float)(d.x - I.trackOX) * 1000.0f / I.trackSpan;
        float Y = (float)(I.trackOY - d.y) * 1000.0f / I.trackSpan;
        int px = s_lmOX + (int)(X * s_lmK), py = s_lmOY + (int)(Y * s_lmK);
        if (px > -20 && px < LMAP_W + 20 && py > -20 && py < LMAP_H + 20) {
          lv_obj_set_pos(s_dot[i], px - 8, py - 8);
          // Names alternate sides down the order, so a train of cars stays readable.
          int lx = px + 10;
          if (d.pos && !(d.pos & 1))
            lx = px - 10 - lv_txt_get_width(d.who.code, strlen(d.who.code), F14, 0, LV_TEXT_FLAG_NONE);
          lv_obj_set_pos(s_dotLbl[i], lx, py - 9);
          setBg(s_dot[i], d.who.colour);
          setText(s_dotLbl[i], d.who.code);
          on = true;
        }
      }
    }
    show(s_dot[i], on);
    show(s_dotLbl[i], on);
  }
  s_fastSeen = L.fastVersion;
}

static void renderMapView(const F1Model &m, const AppStatus &st) {
  const F1Live &L = m.live;
  const F1Info &I = m.info;
  // Order list down the left.
  float ref = 0;
  if (L.kind != F1_RACE && L.nDrv) { bool dim; ref = m.shownBest(L.drv[L.order[0]], &dim); }
  for (int i = 0; i < ROWS; ++i) {
    MapRow &r = s_mrow[i];
    bool on = i < L.nDrv;
    for (lv_obj_t *o : {r.pos, r.bar, r.code, r.tyre, r.gap}) show(o, on);
    if (!on) continue;
    const F1Driver &d = L.drv[L.order[i]];
    char b[20];
    if (d.pos) snprintf(b, sizeof b, "%d", d.pos); else strcpy(b, "-");
    setText(r.pos, b);
    setBg(r.bar, d.who.colour);
    setText(r.code, d.who.code);
    b[0] = d.compound == '?' ? 0 : d.compound; b[1] = 0;
    setText(r.tyre, b);
    setColour(r.tyre, tyreColour(d.compound));
    if (L.kind == F1_RACE) {
      if (d.pos == 1 && L.started) strcpy(b, "LEADER");
      else fmtGap(d.gap, d.gapLaps, b, sizeof b);
    } else {
      bool dim;
      float t = m.shownBest(d, &dim);
      if (!(t > 0)) b[0] = 0;
      else if (i == 0 || !(ref > 0) || t < ref) f1FmtLap(t, b, sizeof b);
      else snprintf(b, sizeof b, "+%.3f", t - ref);
    }
    setText(r.gap, b);
  }

  // Outline, fitted once per circuit.
  const F1SessionInfo *ses = m.sessionByKey(L.sessionKey);
  bool have = ses && I.nTrack > 10 && I.trackCircuit == ses->circuitKey && I.trackW > 0 && I.trackH > 0;
  show(s_lmapSF, have);
  show(s_lmapNone, !have);
  if (!have) {
    s_lmK = 0;
    s_lmCircuit = -1;
    for (int k = 0; k < 3; ++k) { show(s_lmapSeg[k], false); show(s_lmapKey[k], false); }
    for (lv_obj_t *c : s_lmapCorner) show(c, false);
  } else if (s_lmCircuit != I.trackCircuit || s_lmN != I.nTrack || s_lmSec2 != I.trackSec2 ||
             s_lmExtras != I.nCorner * 64 + I.nMsec) {
    float sx = (float)(LMAP_W - 90) / I.trackW, sy = (float)(LMAP_H - 50) / I.trackH;
    s_lmK = sx < sy ? sx : sy;
    s_lmOX = (int)((LMAP_W - 40 - I.trackW * s_lmK) / 2);
    s_lmOY = (int)((LMAP_H - I.trackH * s_lmK) / 2);
    int n = I.nTrack;
    for (int i = 0; i < n; ++i) {
      s_lmapPts[i].x = (lv_coord_t)(s_lmOX + I.trackX[i] * s_lmK);
      s_lmapPts[i].y = (lv_coord_t)(s_lmOY + I.trackY[i] * s_lmK);
    }
    for (int i = n; i <= 2 * n; ++i) s_lmapPts[i] = s_lmapPts[i - n];
    // One line per sector, each running on to the next one's first point;
    // a single grey loop if the sector split is not known.
    bool split = I.trackSec2 > 0 && I.trackSec3 > I.trackSec2 && I.trackSec3 < n;
    int from[4] = {0, split ? I.trackSec2 : n, split ? I.trackSec3 : n, n};
    for (int k = 0; k < 3; ++k) {
      bool on = from[k + 1] > from[k];
      show(s_lmapSeg[k], on);
      show(s_lmapKey[k], split);
      if (!on) continue;
      lv_line_set_points(s_lmapSeg[k], &s_lmapPts[from[k]], from[k + 1] - from[k] + 1);
      lv_obj_set_style_line_color(s_lmapSeg[k], lv_color_hex(split ? SECTOR_COLOUR[k] : C_TRACK), 0);
    }
    lv_obj_set_pos(s_lmapSF, s_lmapPts[0].x - 3, s_lmapPts[0].y - 9);
    // Corner numbers, just off the track.
    for (int c = 0; c < F1_MAX_CORNERS; ++c) {
      bool on = c < I.nCorner;
      show(s_lmapCorner[c], on);
      if (!on) continue;
      char b[6];
      snprintf(b, sizeof b, "%d", I.cornerNum[c]);
      setText(s_lmapCorner[c], b);
      int cx = s_lmOX + (int)(I.cornerX[c] * s_lmK) + I.cornerDX[c] * CORNER_OFF / 100;
      int cy = s_lmOY + (int)(I.cornerY[c] * s_lmK) + I.cornerDY[c] * CORNER_OFF / 100;
      lv_obj_set_pos(s_lmapCorner[c], cx - lv_txt_get_width(b, strlen(b), F14, 0, LV_TEXT_FLAG_NONE) / 2, cy - 8);
    }
    // The stretch of outline each marshal sector covers; shown while it is under yellow.
    for (int k = 0; k < F1_MAX_MSEC; ++k) {
      int a = 0, b = 0;
      if (k < I.nMsec) {
        a = I.msecAt[k];
        b = I.msecAt[(k + 1) % I.nMsec];
        if (b <= a) b += n;
      }
      if (b - a > n / 2) b = a;            // out of order: not a sector
      lv_line_set_points(s_lmapFlag[k], &s_lmapPts[a], b > a ? b - a + 1 : 0);
    }
    s_lmCircuit = I.trackCircuit;
    s_lmN = n;
    s_lmSec2 = I.trackSec2;
    s_lmExtras = I.nCorner * 64 + I.nMsec;
  }
  uint32_t yellow = have && I.nMsec ? L.yellowMask : 0;
  if (I.nMsec < 32) yellow &= (1u << I.nMsec) - 1;
  for (int k = 0; k < F1_MAX_MSEC; ++k) show(s_lmapFlag[k], (yellow >> k) & 1);
  show(s_lmapFlagKey, yellow != 0);
  placeDots(m, st);

  char w[160] = "", one[48];
  if (L.airT > -50) { snprintf(one, sizeof one, "AIR %.0f%s     TRACK %.0f%s", temp(L.airT), tempUnit(), temp(L.trackT), tempUnit()); strlcat(w, one, sizeof w); }
  if (L.humidity >= 0) { snprintf(one, sizeof one, "     HUMIDITY %.0f%%", L.humidity); strlcat(w, one, sizeof w); }
  if (L.windSpeed >= 0) {
    char ws[16];
    fmtWind(L.windSpeed, ws, sizeof ws);
    snprintf(one, sizeof one, "     WIND %s %s", ws, L.windDir >= 0 ? compass(L.windDir) : "");
    strlcat(w, one, sizeof w);
  }
  if (L.pressure > 0) { snprintf(one, sizeof one, "     %.0f mbar", L.pressure); strlcat(w, one, sizeof w); }
  if (L.rain) strlcat(w, "     RAIN", sizeof w);
  setText(s_wx, w);
  setText(s_note, L.note);
}

// ---- live: sectors -----------------------------------------------------------

static uint32_t paceColour(float v, float personal, float session) {
  if (session > 0 && v <= session + 0.0005f) return C_PURPLE;
  if (personal > 0 && v <= personal + 0.0005f) return C_GREEN;
  return C_YELLOW;
}

static void drawSegs(lv_event_t *e) {
  int row = (int)(intptr_t)lv_event_get_user_data(e);
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target(e), &a);
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_opa = LV_OPA_COVER;
  d.radius = 1;
  int k = 0;
  for (int sct = 0; sct < 3; ++sct) {
    int n = s_segN[row][sct];
    if (!n) continue;
    int w = (72 - (n - 1) * 2) / n;
    if (w < 2) w = 2;
    for (int i = 0; i < n; ++i, ++k) {
      uint8_t c = s_segCopy[row][k];
      d.bg_color = lv_color_hex(c == SEG_PURPLE ? C_PURPLE : c == SEG_GREEN ? C_GREEN :
                                c == SEG_YELLOW ? C_YELLOW : c == SEG_PIT ? 0x3B82F6 : C_OFF);
      lv_area_t r;
      r.x1 = (lv_coord_t)(a.x1 + sct * 74 + i * (w + 2));
      r.x2 = (lv_coord_t)(r.x1 + w - 1);
      r.y1 = a.y1;
      r.y2 = a.y2;
      lv_draw_rect(ctx, &d, &r);
    }
  }
}

// --- Tyre strategy: one row per driver, a bar per set of tyres along the laps ---
#define TY_Y0  22            // first row inside the panel; the lap scale sits above it
#define TY_ROW 21
#define TY_X0  124           // lap 0 ...
#define TY_X1  1008          // ... and the right-hand end of the scale
struct TyreRow { uint8_t pos, nStint; uint16_t lap; uint32_t colour; char code[4]; F1Stint st[F1_MAX_STINTS]; };
static TyreRow s_tyRows[ROWS];             // what the chart was last drawn from
static int s_tyN = -1, s_tyAxis = 0;
static lv_obj_t *s_pTyre, *s_tyChart;

// Drawn straight onto one object: 22 rows of up to 8 bars would be a few
// hundred objects otherwise.
static void drawTyres(lv_event_t *e) {
  if (s_tyN <= 0 || s_tyAxis <= 0) return;
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target(e), &a);
  lv_draw_rect_dsc_t rd;
  lv_draw_rect_dsc_init(&rd);
  rd.bg_opa = LV_OPA_COVER;
  lv_draw_label_dsc_t ld;
  lv_draw_label_dsc_init(&ld);
  auto text = [&](int x, int y, int w, const char *t, const lv_font_t *f, uint32_t col, lv_text_align_t al) {
    ld.font = f;
    ld.color = lv_color_hex(col);
    ld.align = al;
    lv_area_t r = {(lv_coord_t)(a.x1 + x), (lv_coord_t)(a.y1 + y), (lv_coord_t)(a.x1 + x + w - 1),
                   (lv_coord_t)(a.y1 + y + lv_font_get_line_height(f) - 1)};
    lv_draw_label(ctx, &ld, &r, t, nullptr);
  };
  auto box = [&](int x1, int y1, int x2, int y2, uint32_t col, int radius) {
    rd.bg_color = lv_color_hex(col);
    rd.radius = (lv_coord_t)radius;
    lv_area_t r = {(lv_coord_t)(a.x1 + x1), (lv_coord_t)(a.y1 + y1), (lv_coord_t)(a.x1 + x2), (lv_coord_t)(a.y1 + y2)};
    lv_draw_rect(ctx, &rd, &r);
  };
  auto lapX = [](int lap) { return TY_X0 + (int)((long)lap * (TY_X1 - TY_X0) / s_tyAxis); };
  char b[12];
  // Lap scale with a faint line down the rows.
  int every = s_tyAxis <= 20 ? 5 : 10;
  text(54, 2, 60, "LAP", F14, C_DIM, LV_TEXT_ALIGN_LEFT);
  for (int lap = 0; lap <= s_tyAxis; lap += every) {
    int x = lapX(lap);
    box(x, TY_Y0 - 2, x, TY_Y0 + s_tyN * TY_ROW - 2, C_OFF, 0);
    snprintf(b, sizeof b, "%d", lap);
    text(x - 20, 2, 40, b, F14, C_DIM, LV_TEXT_ALIGN_CENTER);
  }
  for (int i = 0; i < s_tyN; ++i) {
    const TyreRow &r = s_tyRows[i];
    int y = TY_Y0 + i * TY_ROW;
    if (r.pos) snprintf(b, sizeof b, "%d", r.pos); else strcpy(b, "-");
    text(6, y, 30, b, F16, C_DIM, LV_TEXT_ALIGN_RIGHT);
    box(42, y + 2, 45, y + 17, r.colour, 1);
    text(54, y, 64, r.code, F16, C_TEXT, LV_TEXT_ALIGN_LEFT);
    for (int k = 0; k < r.nStint; ++k) {
      const F1Stint &st = r.st[k];
      if (!st.lapStart) continue;
      int end = r.lap;                           // the newest set is still on the car
      for (int j = k + 1; j < r.nStint; ++j)
        if (r.st[j].lapStart) { end = r.st[j].lapStart - 1; break; }
      if (end < st.lapStart) continue;
      int x1 = lapX(st.lapStart - 1) + 1, x2 = lapX(end) - 1;
      if (x2 < x1) x2 = x1;
      box(x1, y + 2, x2, y + 18, tyreColour(st.compound), 3);
      // Laps on that set, with the compound's letter where there is room.
      int laps = st.age0 + end - st.lapStart + 1, w = x2 - x1 + 1;
      snprintf(b, sizeof b, "%c %d", st.compound, laps);
      if (st.compound == '?' || lv_txt_get_width(b, strlen(b), F14, 0, LV_TEXT_FLAG_NONE) + 8 > w)
        snprintf(b, sizeof b, "%d", laps);
      if (lv_txt_get_width(b, strlen(b), F14, 0, LV_TEXT_FLAG_NONE) + 6 <= w)
        text(x1, y + 2, w, b, F14, C_BG, LV_TEXT_ALIGN_CENTER);
    }
  }
}

static void renderTyres(const F1Model &m) {
  const F1Live &L = m.live;
  static TyreRow now[ROWS];
  memset(now, 0, sizeof now);
  int n = L.nDrv < ROWS ? L.nDrv : ROWS, top = 0;
  for (int i = 0; i < n; ++i) {
    const F1Driver &d = L.drv[L.order[i]];
    TyreRow &r = now[i];
    r.pos = d.pos;
    r.nStint = d.nStint;
    r.lap = d.lapNo ? d.lapNo : 1;
    r.colour = d.who.colour;
    snprintf(r.code, sizeof r.code, "%s", d.who.code);
    memcpy(r.st, d.stint, sizeof r.st);
    if (r.lap > top) top = r.lap;
  }
  int axis = (top + 4) / 5 * 5;                  // the scale ends on the next multiple of 5 laps
  if (axis < 10) axis = 10;
  if (n != s_tyN || axis != s_tyAxis || memcmp(now, s_tyRows, sizeof now)) {
    memcpy(s_tyRows, now, sizeof now);
    s_tyN = n;
    s_tyAxis = axis;
    lv_obj_invalidate(s_tyChart);
  }
  setText(s_note, "Tyre stints by lap   |   number: laps on that set");
}

static void renderSectors(const F1Model &m) {
  const F1Live &L = m.live;
  int n = L.nDrv > ROWS ? ROWS : L.nDrv;
  int perCol = n <= PER_COL ? (n + 1) / 2 : PER_COL;
  if (perCol < 1) perCol = 1;
  for (int i = 0; i < ROWS; ++i) {
    SecRow &r = s_srow[i];
    show(r.box, i < n);
    if (i >= n) continue;
    const F1Driver &d = L.drv[L.order[i]];
    lv_obj_set_pos(r.box, 12 + (i / perCol) * (COL_W + 4), 8 + (i % perCol) * ROW_H);
    char b[16];
    if (d.pos) snprintf(b, sizeof b, "%d", d.pos); else strcpy(b, "-");
    setText(r.pos, b);
    setBg(r.bar, d.who.colour);
    setText(r.name, d.who.code);
    for (int k = 0; k < 3; ++k) {
      if (d.sector[k] > 0) snprintf(b, sizeof b, "%.3f", d.sector[k]); else b[0] = 0;
      setText(r.s[k], b);
      setColour(r.s[k], paceColour(d.sector[k], d.bestSector[k], L.bestSector[k]));
    }
    f1FmtLap(d.lastLap, b, sizeof b);
    setText(r.lap, b);
    setColour(r.lap, L.fastest > 0 && d.lastLap > 0 && d.lastLap <= L.fastest + 0.0005f ? C_PURPLE : C_TEXT);
    if (d.trap) snprintf(b, sizeof b, "%d", speed(d.trap)); else b[0] = 0;
    setText(r.trap, b);
    setColour(r.trap, d.trap && d.trap >= L.bestTrap ? C_PURPLE : d.trap && d.trap >= d.bestTrap ? C_GREEN : C_DIM);
    if (memcmp(s_segCopy[i], d.seg, F1_MAX_SEGS) || memcmp(s_segN[i], d.nSeg, 3)) {
      memcpy(s_segCopy[i], d.seg, F1_MAX_SEGS);
      memcpy(s_segN[i], d.nSeg, 3);
      lv_obj_invalidate(r.strip);
    }
  }
  char note[80];
  snprintf(note, sizeof note, "trap in %s  |  purple: session best  |  green: personal best", speedUnit());
  setText(s_note, note);
}

// ---- live: one car -----------------------------------------------------------

static const char *compoundName(char c) {
  switch (c) {
    case 'S': return "SOFT";
    case 'M': return "MEDIUM";
    case 'H': return "HARD";
    case 'I': return "INTERMEDIATE";
    case 'W': return "WET";
    default:  return "";
  }
}

// The fast-changing part, also refreshed between the half-second ticks.
static void renderTelemetry(const F1Model &m, const AppStatus &st) {
  const F1Driver *d = m.driver(s_focus);
  bool fresh = d && d->carMs && st.dataNowMs - d->carMs < 6000;
  char b[16];
  if (fresh) snprintf(b, sizeof b, "%d", speed(d->speed)); else strcpy(b, "--");
  setText(s_dSpeed, b);
  if (!fresh) strcpy(b, "-");
  else if (d->gear <= 0) strcpy(b, "N");
  else snprintf(b, sizeof b, "%d", d->gear);
  setText(s_dGear, b);
  if (fresh) snprintf(b, sizeof b, "%d", d->rpm); else b[0] = 0;
  setText(s_dRpm, b);
  int rpm = fresh ? d->rpm : 0, thr = fresh ? d->throttle : 0, brk = fresh ? d->brake : 0;
  if (lv_bar_get_value(s_dRpmBar) != rpm) lv_bar_set_value(s_dRpmBar, rpm, LV_ANIM_OFF);
  if (lv_bar_get_value(s_dThr) != thr) lv_bar_set_value(s_dThr, thr, LV_ANIM_OFF);
  if (lv_bar_get_value(s_dBrk) != brk) lv_bar_set_value(s_dBrk, brk, LV_ANIM_OFF);
  s_fastSeen = m.live.fastVersion;
}

static void renderDriver(const F1Model &m, const AppStatus &st) {
  const F1Live &L = m.live;
  const F1Driver *d = m.driver(s_focus);
  if (!d) return;
  char b[120], t[20];
  uint16_t hw = 0, hh = 0;
  const uint8_t *px = headGet(s_focus, &hw, &hh);
  if (px != s_headPx) {
    s_headPx = px;
    if (px) {
      s_headDsc.header.always_zero = 0;
      s_headDsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
      s_headDsc.header.w = hw;
      s_headDsc.header.h = hh;
      s_headDsc.data_size = (uint32_t)hw * hh * 3;
      s_headDsc.data = px;
      lv_img_set_src(s_dImg, &s_headDsc);
      lv_obj_center(s_dImg);
      lv_obj_invalidate(s_dImg);
    }
    show(s_dImg, px != nullptr);
  }
  setBg(s_dBar, d->who.colour);
  if (d->pos) snprintf(b, sizeof b, "P%d", d->pos); else strcpy(b, "-");
  setText(s_dPos, b);
  setText(s_dCode, d->who.code);
  snprintf(b, sizeof b, "%s  #%d", d->who.last, d->who.num);
  setText(s_dName, b);
  setText(s_dTeam, d->who.team);
  if (L.kind == F1_RACE) {
    if (d->pos == 1 && L.started) strcpy(b, "LEADER");
    else {
      char g[16], iv[16];
      fmtGap(d->gap, d->gapLaps, g, sizeof g);
      fmtGap(d->interval, d->intLaps, iv, sizeof iv);
      snprintf(b, sizeof b, "Leader  %s\nAhead   %s", g, iv);
    }
  } else {
    bool dim;
    f1FmtLap(m.shownBest(*d, &dim), t, sizeof t);
    snprintf(b, sizeof b, "Best  %s", t[0] ? t : "no time");
  }
  setText(s_dGap, b);
  if (d->compound == '?') b[0] = 0;
  else if (L.kind == F1_RACE)
    snprintf(b, sizeof b, "%s   %d lap%s   %d stop%s", compoundName(d->compound), d->tyreAge,
             d->tyreAge == 1 ? "" : "s", d->stops, d->stops == 1 ? "" : "s");
  else snprintf(b, sizeof b, "%s   %d lap%s", compoundName(d->compound), d->tyreAge, d->tyreAge == 1 ? "" : "s");
  setText(s_dTyre, b);
  setColour(s_dTyre, tyreColour(d->compound));
  if (d->pitLap && d->pitLane > 0) {
    if (d->pitStop > 0)
      snprintf(b, sizeof b, "Last stop: lap %d\n%.1f s in the pit lane, %.1f s stationary", d->pitLap, d->pitLane, d->pitStop);
    else snprintf(b, sizeof b, "Last stop: lap %d\n%.1f s in the pit lane", d->pitLap, d->pitLane);
  } else b[0] = 0;
  setText(s_dPit, b);

  for (int k = 0; k < 3; ++k) {
    if (d->sector[k] > 0) snprintf(b, sizeof b, "%.3f", d->sector[k]); else strcpy(b, "-");
    setText(s_dSec[k], b);
    setColour(s_dSec[k], d->sector[k] > 0 ? paceColour(d->sector[k], d->bestSector[k], L.bestSector[k]) : C_DIM);
    if (d->bestSector[k] > 0) snprintf(b, sizeof b, "best %.3f", d->bestSector[k]); else b[0] = 0;
    setText(s_dSecBest[k], b);
  }
  f1FmtLap(d->lastLap, b, sizeof b);
  setText(s_dLast, b[0] ? b : "-");
  setColour(s_dLast, L.fastest > 0 && d->lastLap > 0 && d->lastLap <= L.fastest + 0.0005f ? C_PURPLE : C_TEXT);
  bool dim;
  f1FmtLap(m.shownBest(*d, &dim), b, sizeof b);
  setText(s_dBest, b[0] ? b : "-");
  if (d->trap) snprintf(b, sizeof b, "%d %s", speed(d->trap), speedUnit()); else strcpy(b, "-");
  setText(s_dTrap, b);
  setColour(s_dTrap, d->trap && d->trap >= L.bestTrap ? C_PURPLE : C_TEXT);
  renderTelemetry(m, st);
  setText(s_note, L.note);
}

// ---- countdown screen ------------------------------------------------------

#define MAP_W 380
#define MAP_H 220

// Fit the outline into the map box, keeping its proportions.
static void renderMap(const F1Model &m, const F1SessionInfo *nx) {
  const F1Info &I = m.info;
  bool have = nx && I.nTrack > 10 && I.trackCircuit == nx->circuitKey;
  show(s_map, have);
  if (!have || (s_mapCircuit == I.trackCircuit && s_mapVersion == I.nTrack)) return;
  float sx = (float)(MAP_W - 16) / I.trackW, sy = (float)(MAP_H - 16) / I.trackH;
  float k = sx < sy ? sx : sy;
  int ox = (int)((MAP_W - I.trackW * k) / 2), oy = (int)((MAP_H - I.trackH * k) / 2);
  int n = I.nTrack;
  for (int i = 0; i < n; ++i) {
    s_mapPts[i].x = (lv_coord_t)(ox + I.trackX[i] * k);
    s_mapPts[i].y = (lv_coord_t)(oy + I.trackY[i] * k);
  }
  s_mapPts[n] = s_mapPts[0];                       // close the loop
  lv_line_set_points(s_mapLine, s_mapPts, n + 1);
  lv_obj_set_pos(s_mapDot, s_mapPts[0].x - 6, s_mapPts[0].y - 6);
  const F1MeetingInfo *mt = m.meetingByKey(nx->meetingKey);
  char name[32];
  strlcpy(name, mt ? mt->circuit : "", sizeof name);
  upper(name);
  setText(s_mapName, name);
  s_mapCircuit = I.trackCircuit;
  s_mapVersion = n;
}


static void localFmt(int64_t ms, const char *fmt, char *out, size_t n) {
  time_t t = (time_t)(ms / 1000);
  struct tm lt;
  localtime_r(&t, &lt);
  strftime(out, n, fmt, &lt);
}

static void renderNext(const F1Model &m, const AppStatus &st) {
  int64_t now = appWallMs();
  const F1SessionInfo *nx = m.nextSession(now);
  char b[96];
  if (!nx) {
    setText(s_nxKicker, "FORMULA 1");
    setText(s_nxMeeting, m.info.nSessions ? "Season complete" : "No calendar yet");
    setText(s_nxSession, "");
    setText(s_nxCount, "");
    setText(s_nxTimes, "");
    setText(s_nxNames, "");
  } else {
    const F1MeetingInfo *mt = m.meetingByKey(nx->meetingKey);
    setText(s_nxKicker, "NEXT SESSION");
    setText(s_nxMeeting, mt ? mt->name : "Formula 1");
    char when[40];
    localFmt(nx->startMs, "%a %d %b  %H:%M", when, sizeof when);
    snprintf(b, sizeof b, "%s  |  %s", nx->name, when);
    setText(s_nxSession, b);
    int64_t left = (nx->startMs - now) / 1000;
    if (left < 0) left = 0;
    int d = (int)(left / 86400), h = (int)(left / 3600 % 24), mi = (int)(left / 60 % 60), s = (int)(left % 60);
    if (d > 0) snprintf(b, sizeof b, "%dd %02d:%02d:%02d", d, h, mi, s);
    else snprintf(b, sizeof b, "%02d:%02d:%02d", h, mi, s);
    setText(s_nxCount, b);
    // This meeting's timetable in local time.
    char times[200] = "", names[260] = "";
    for (int i = 0; i < m.info.nSessions; ++i) {
      const F1SessionInfo &s2 = m.info.sessions[i];
      if (s2.meetingKey != nx->meetingKey || s2.cancelled) continue;
      char w2[24];
      localFmt(s2.startMs, "%a %H:%M", w2, sizeof w2);
      strlcat(times, w2, sizeof times);
      strlcat(times, "\n", sizeof times);
      strlcat(names, s2.name, sizeof names);
      strlcat(names, s2.endMs < now ? "   (done)" : (s2.key == nx->key ? "   " LV_SYMBOL_LEFT : ""), sizeof names);
      strlcat(names, "\n", sizeof names);
    }
    setText(s_nxTimes, times);
    setText(s_nxNames, names);
  }
  renderMap(m, nx);
  if (st.authNeeded) setText(s_nxStatus, "Live timing needs an OpenF1 login - see the set-up page");
  else {
    char clk[16];
    localFmt(now, "%H:%M", clk, sizeof clk);
    snprintf(b, sizeof b, "%s   %s", clk, st.msg);
    setText(s_nxStatus, b);
  }
}

// ---- alerts ------------------------------------------------------------------

static void showAlert(const F1Model &m, const F1Alert &a) {
  const char *big = "", *sub = "";
  char sub2[80] = "";
  uint32_t bg = C_YELLOW, fg = 0x000000;
  switch (a.kind) {
    case ALERT_START:      big = "LIGHTS OUT"; sub = "The race is under way"; bg = C_GREEN; break;
    case ALERT_GREEN:      big = "GREEN FLAG"; sub = "Track clear"; bg = C_GREEN; break;
    case ALERT_SC:         big = "SAFETY CAR"; sub = "No overtaking"; break;
    case ALERT_SC_ENDING:  big = "SAFETY CAR"; sub = "In this lap"; break;
    case ALERT_VSC:        big = "VIRTUAL SAFETY CAR"; sub = "No overtaking"; break;
    case ALERT_VSC_ENDING: big = "VIRTUAL SAFETY CAR"; sub = "Ending"; break;
    case ALERT_RED:        big = "RED FLAG"; sub = "Session stopped"; bg = C_RED; fg = 0xFFFFFF; break;
    case ALERT_CHEQUERED: {
      big = "CHEQUERED FLAG";
      bg = 0xF2F4F7;
      static char win[40];
      const F1Live &L = m.live;
      if (L.nDrv) snprintf(win, sizeof win, "Winner: %s", L.drv[L.order[0]].who.code);
      else win[0] = 0;
      sub = win;
      for (int i = 1; i < 5 && i < L.nDrv; ++i) {
        char one[16];
        snprintf(one, sizeof one, "%s%d %s", i > 1 ? "    " : "", i + 1, L.drv[L.order[i]].who.code);
        strlcat(sub2, one, sizeof sub2);
      }
      break;
    }
    default: return;
  }
  lv_obj_set_style_bg_color(s_alert, lv_color_hex(bg), 0);
  setText(s_alertBig, big);
  setText(s_alertSub, sub);
  setText(s_alertSub2, sub2);
  for (lv_obj_t *l : {s_alertBig, s_alertSub, s_alertSub2}) setColour(l, fg);
  show(s_alert, true);
  s_alertUntil = millis() + ALERT_SHOW_MS;
  if (a.kind == ALERT_START) soundRaceStart();
}

static void showToast(F1Model &m, const F1Alert &a) {
  F1Driver *x = m.driver(a.a), *y = m.driver(a.b);
  if (!x) return;
  char b[112], t[16];
  uint32_t bar = x->who.colour;
  switch (a.kind) {
    case ALERT_OVERTAKE:
      if (!y) return;
      snprintf(b, sizeof b, "%s  " LV_SYMBOL_UP "  P%d     passes %s", x->who.code, a.pos, y->who.code);
      break;
    case ALERT_PIT:
      if (x->pitStop > 0)
        snprintf(b, sizeof b, "%s   PIT STOP   lap %d     %.1f s in the pit lane, %.1f s stationary", x->who.code,
                 x->pitLap, x->pitLane, x->pitStop);
      else snprintf(b, sizeof b, "%s   PIT STOP   lap %d     %.1f s in the pit lane", x->who.code, x->pitLap, x->pitLane);
      break;
    case ALERT_RADIO:
      snprintf(b, sizeof b, LV_SYMBOL_VOLUME_MAX "   TEAM RADIO   %s  %s", x->who.code, x->who.last);
      break;
    case ALERT_FASTEST:
      // The alert carries no time, so only the holder's strip can be right:
      // one poll can raise three of these, and each used to show the last
      // one's time beside a different driver.
      if (a.a != m.live.fastestNum) return;
      f1FmtLap(m.live.fastest, t, sizeof t);
      snprintf(b, sizeof b, "FASTEST LAP   %s   %s", x->who.code, t);
      bar = C_PURPLE;
      break;
    default: return;
  }
  setText(s_toastLbl, b);
  setBg(s_toastBar, bar);
  show(s_toast, true);
  s_toastUntil = millis() + TOAST_SHOW_MS;
}

// ---- tick --------------------------------------------------------------------

static bool viewAvailable(const F1Model &m, int v) {
  switch (v) {
    case V_GRID: {
#ifdef GRID_TEST_MEETING
      return m.info.nGrid > 0;
#else
      const F1SessionInfo *nx = m.nextSession(appWallMs());
      return m.info.nGrid > 0 && nx && nx->kind == F1_RACE && nx->meetingKey == m.info.gridMeeting;
#endif
    }
    case V_RESULT:  return m.info.nResult > 0;
    case V_DRIVERS: return m.info.nDrvStand > 0;
    case V_TEAMS:   return m.info.nTeamStand > 0;
    default:        return true;
  }
}

static void goView(int v) {
  if (v == LV_DRIVER && s_liveView != LV_DRIVER) s_backView = s_liveView;
  s_liveView = v;
}

static void tick(lv_timer_t *) {
  uint32_t now = millis();
  if (s_alertUntil && (int32_t)(now - s_alertUntil) >= 0) { show(s_alert, false); s_alertUntil = 0; }
  if (s_toastUntil && (int32_t)(now - s_toastUntil) >= 0) { show(s_toast, false); s_toastUntil = 0; }

  if (!appLock(0)) return;            // the network task is mid-update; next tick
  F1Model &m = appModel();
  const AppStatus &st = appStatus();

  char clk[16] = "";
  if (appWallMs()) localFmt(appWallMs(), "%H:%M", clk, sizeof clk);
  setText(s_clock, clk);

  if (st.mode == APP_BOOT) {
    show(s_boot, true); show(s_table, false); show(s_next, false);
    lv_label_set_text_fmt(lv_obj_get_child(s_boot, 1), "%s", st.msg);
  } else if (st.mode == APP_LIVE) {
    show(s_boot, false); show(s_table, true); show(s_next, false);
    const F1Live &L = m.live;
    s_orderN = L.nDrv;
    for (int i = 0; i < L.nDrv; ++i) s_orderNum[i] = L.drv[L.order[i]].who.num;
    if (s_cmdView >= 0) { goView(s_cmdView % LV_COUNT); s_cmdView = -1; }
    if (s_cmdDriver >= 0) { s_focus = (uint8_t)s_cmdDriver; goView(LV_DRIVER); s_cmdDriver = -1; }
    if (!m.driver(s_focus)) s_focus = L.nDrv ? s_orderNum[0] : 0;
    if (s_liveView == LV_DRIVER && !s_focus) s_liveView = LV_TOWER;
    liveHeader(m, st);
    show(s_pMap, s_liveView == LV_MAP);
    show(s_pSec, s_liveView == LV_SECTORS);
    show(s_pTyre, s_liveView == LV_TYRES);
    show(s_pDrv, s_liveView == LV_DRIVER);
    if (s_liveView != LV_TOWER) for (Row &r : s_rows) show(r.box, false);
    switch (s_liveView) {
      case LV_MAP:     renderMapView(m, st); break;
      case LV_SECTORS: renderSectors(m); break;
      case LV_TYRES:   renderTyres(m); break;
      case LV_DRIVER:  renderDriver(m, st); break;
      default:         renderLive(m); break;
    }
    for (int i = 0; i < LV_COUNT; ++i) {
      show(s_tabs[i], true);
      setBg(s_tabs[i], i == s_liveView ? C_RED : C_ROW);
      setColour(s_tabLbl[i], i == s_liveView ? 0xFFFFFF : C_DIM);
    }
    show(s_streamDot, true);                 // green: pushed live; grey: polling
    setBg(s_streamDot, st.stream ? C_GREEN : 0x5A6270);
    F1Alert a;
    if (s_cmdAlert > 0 && L.nDrv > 1) {              // bench: show one alert, made up from P1 and P2
      a = {(uint8_t)s_cmdAlert, s_orderNum[0], s_orderNum[1], 1};
      if (a.kind >= ALERT_OVERTAKE) showToast(m, a); else showAlert(m, a);
    }
    s_cmdAlert = -1;
    // One thing at a time: wait for the current alert / strip to clear.
    while (!s_alertUntil && !s_toastUntil && m.popAlert(a)) {
      if (a.kind >= ALERT_OVERTAKE) showToast(m, a);
      else showAlert(m, a);
    }
  } else {
    show(s_pMap, false); show(s_pSec, false); show(s_pTyre, false); show(s_pDrv, false);
    for (lv_obj_t *t : s_tabs) show(t, false);
    show(s_streamDot, false);
    if (s_cmdIdle >= 0) { s_view = s_cmdIdle % V_COUNT; s_viewSince = now; s_cmdIdle = -1; }
    if (s_skip || (IDLE_ROTATE_S > 0 && now - s_viewSince > IDLE_ROTATE_S * 1000UL)) {   // no change on its own at 0
      s_skip = false;
      for (int i = 0; i < V_COUNT; ++i) {
        s_view = (s_view + 1) % V_COUNT;
        if (viewAvailable(m, s_view)) break;
      }
      s_viewSince = now;
    }
    if (!viewAvailable(m, s_view)) s_view = V_NEXT;
    show(s_boot, false);
    show(s_table, s_view != V_NEXT);
    show(s_next, s_view == V_NEXT);
    switch (s_view) {
      case V_GRID:    renderGrid(m); break;
      case V_RESULT:  renderResult(m); break;
      case V_DRIVERS: renderDrivers(m); break;
      case V_TEAMS:   renderTeams(m); break;
      default:        renderNext(m, st); break;
    }
  }
  s_isLive = st.mode == APP_LIVE;
  appWantFast(s_isLive && s_liveView == LV_MAP, s_isLive && s_liveView == LV_DRIVER ? s_focus : 0);
  appUnlock();
}

// Car positions and telemetry arrive several times a second.
static void fastTick(lv_timer_t *) {
  if (!s_isLive || (s_liveView != LV_MAP && s_liveView != LV_DRIVER)) return;
  if (!appLock(0)) return;
  const F1Model &m = appModel();
  if (m.live.fastVersion != s_fastSeen) {
    if (s_liveView == LV_MAP) placeDots(m, appStatus());
    else renderTelemetry(m, appStatus());
  }
  appUnlock();
}

// Live screens. The tabs in the footer pick a screen (the title bar steps
// to the next one); a driver's row on any screen opens that car; on the car
// screen the sides step through the field and the middle goes back.
static void onTap(lv_event_t *) {
  if (!s_isLive) { s_skip = true; return; }          // between sessions: next screen
  lv_point_t p = {0, 0};
  lv_indev_t *in = lv_indev_get_act();
  if (in) lv_indev_get_point(in, &p);
  if (p.y >= ROW_Y0 + PER_COL * ROW_H) {             // footer (a finger-width taller than it looks)
    int t = (p.x - TAB_X + 3) / TAB_W;
    if (p.x >= TAB_X - 3 && t < LV_COUNT) goView(t);
    return;
  }
  if (p.y < 62) { goView((s_liveView + 1) % LV_COUNT); return; }

  int car = 0;
  if (s_liveView == LV_TOWER || s_liveView == LV_SECTORS) {
    int perCol = s_orderN <= PER_COL ? (s_orderN + 1) / 2 : PER_COL;
    if (perCol < 1) perCol = 1;
    int row = (p.y - ROW_Y0) / ROW_H, i = (p.x >= 12 + COL_W ? perCol : 0) + row;
    if (p.y >= ROW_Y0 && row < perCol && i < s_orderN) car = s_orderNum[i];
  } else if (s_liveView == LV_MAP && p.x < LMAP_X) {
    int i = (p.y - 70) / 22;
    if (p.y >= 70 && i < s_orderN) car = s_orderNum[i];
  } else if (s_liveView == LV_TYRES) {
    int i = (p.y - 62 - TY_Y0) / TY_ROW;
    if (p.y >= 62 + TY_Y0 && i < s_orderN) car = s_orderNum[i];
  }
  if (car) { s_focus = (uint8_t)car; goView(LV_DRIVER); return; }

  if (s_liveView == LV_DRIVER) {
    if (s_orderN && (p.x < 300 || p.x > SCR_W - 300)) {
      int at = 0;
      for (int i = 0; i < s_orderN; ++i) if (s_orderNum[i] == s_focus) at = i;
      at = (at + (p.x < 300 ? s_orderN - 1 : 1)) % s_orderN;
      s_focus = s_orderNum[at];
    } else {
      s_liveView = s_backView == LV_DRIVER ? LV_TOWER : s_backView;
    }
  }
}

void uiDebug(const char *cmd) {
  int v = atoi(cmd + 1);
  if (cmd[0] == 'V') s_cmdView = v;
  else if (cmd[0] == 'D') s_cmdDriver = v;
  else if (cmd[0] == 'I') s_cmdIdle = v;
  else if (cmd[0] == 'A') s_cmdAlert = v;
}

// ---- construction --------------------------------------------------------------

void uiBegin() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scr, onTap, LV_EVENT_CLICKED, nullptr);

  // Table view
  s_table = mkBox(scr, 0, 0, SCR_W, SCR_H, C_BG);
  lv_obj_t *hdr = mkBox(s_table, 0, 0, SCR_W, 62, C_PANEL);
  mkBox(hdr, 0, 0, 6, 62, C_RED);
  s_title = mkLabel(hdr, 20, 18, 560, F24, C_TEXT);
  s_center = mkLabel(hdr, 0, 8, 0, F40, C_TEXT);
  lv_obj_align(s_center, LV_ALIGN_TOP_LEFT, 590, 8);
  s_pill = mkBox(hdr, SCR_W - 232, 8, 220, 30, C_GREEN, 15);
  s_pillLbl = mkLabel(s_pill, 0, 0, 0, F20, 0x000000);
  lv_obj_center(s_pillLbl);
  s_sub = mkLabel(hdr, SCR_W - 252, 42, 240, F14, C_DIM, LV_TEXT_ALIGN_RIGHT);
  for (int i = 0; i < ROWS; ++i) {
    Row &r = s_rows[i];
    r.box = mkBox(s_table, 12, ROW_Y0 + i * ROW_H, COL_W - 4, ROW_H - 4, (i % 2) ? C_ROW : C_PANEL, 4);
    r.pos = mkLabel(r.box, 0, 7, 40, F24, C_DIM, LV_TEXT_ALIGN_RIGHT);
    r.bar = mkBox(r.box, 48, 5, 6, ROW_H - 14, 0x808080, 2);
    r.name = mkLabel(r.box, 64, 5, 84, F28, C_TEXT);
    r.delta = mkLabel(r.box, 152, 12, 54, F16, C_UP);
    r.ring = mkBox(r.box, 208, 6, 28, 28, C_BG, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(r.ring, 3, 0);
    lv_obj_set_style_border_color(r.ring, lv_color_hex(C_DIM), 0);
    r.tyre = mkLabel(r.ring, 0, 0, 0, F14, C_TEXT);
    lv_obj_center(r.tyre);
    r.age = mkLabel(r.box, 240, 13, 30, F14, C_DIM);
    r.c1 = mkLabel(r.box, 262, 7, 124, F24, C_TEXT, LV_TEXT_ALIGN_RIGHT);
    r.c2 = mkLabel(r.box, 390, 10, 98, F20, C_DIM, LV_TEXT_ALIGN_RIGHT);
  }
  lv_obj_t *ftr = mkBox(s_table, 0, SCR_H - 38, SCR_W, 38, C_PANEL);
  s_note = mkLabel(ftr, 16, 10, TAB_X - 32, F16, C_DIM);
  static const char *const TABS[LV_COUNT] = {"TOWER", "MAP", "SECTORS", "TYRES", "CAR"};
  for (int i = 0; i < LV_COUNT; ++i) {
    s_tabs[i] = mkBox(ftr, TAB_X + i * TAB_W, 5, TAB_W - 6, 28, C_ROW, 6);
    s_tabLbl[i] = mkLabel(s_tabs[i], 0, 0, 0, F14, C_DIM);
    lv_label_set_text(s_tabLbl[i], TABS[i]);
    lv_obj_center(s_tabLbl[i]);
    show(s_tabs[i], false);
  }
  s_streamDot = mkBox(ftr, SCR_W - 96, 14, 10, 10, C_OFF, LV_RADIUS_CIRCLE);
  show(s_streamDot, false);
  s_clock = mkLabel(ftr, SCR_W - 80, 8, 68, F20, C_TEXT, LV_TEXT_ALIGN_RIGHT);

  // Live: map. Running order on the left, the circuit with the cars on the right.
  s_pMap = mkBox(s_table, 0, 62, SCR_W, 500, C_BG);
  for (int i = 0; i < ROWS; ++i) {
    MapRow &r = s_mrow[i];
    int y = 8 + i * 22;
    r.pos = mkLabel(s_pMap, 8, y, 28, F16, C_DIM, LV_TEXT_ALIGN_RIGHT);
    r.bar = mkBox(s_pMap, 42, y + 2, 4, 16, 0x808080, 1);
    r.code = mkLabel(s_pMap, 54, y, 50, F16, C_TEXT);
    r.tyre = mkLabel(s_pMap, 108, y, 20, F16, C_DIM);
    r.gap = mkLabel(s_pMap, 132, y, 160, F16, C_DIM, LV_TEXT_ALIGN_RIGHT);
  }
  s_lmap = mkBox(s_pMap, LMAP_X, 6, LMAP_W, LMAP_H, C_ROW, 6);
  for (int k = 0; k < 3; ++k) {
    s_lmapSeg[k] = lv_line_create(s_lmap);
    lv_obj_set_style_line_width(s_lmapSeg[k], 7, 0);
    lv_obj_set_style_line_color(s_lmapSeg[k], lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_line_rounded(s_lmapSeg[k], true, 0);
    show(s_lmapSeg[k], false);
    char name[12];
    snprintf(name, sizeof name, "SECTOR %d", k + 1);
    s_lmapKey[k] = mkLabel(s_lmap, LMAP_W - 96, 12 + k * 22, 0, F14, SECTOR_COLOUR[k]);
    lv_label_set_text(s_lmapKey[k], name);
    show(s_lmapKey[k], false);
  }
  for (int k = 0; k < F1_MAX_MSEC; ++k) {
    s_lmapFlag[k] = lv_line_create(s_lmap);
    lv_obj_set_style_line_width(s_lmapFlag[k], 15, 0);
    lv_obj_set_style_line_color(s_lmapFlag[k], lv_color_hex(C_FLAG), 0);
    lv_obj_set_style_line_rounded(s_lmapFlag[k], true, 0);
    show(s_lmapFlag[k], false);
  }
  s_lmapFlagKey = mkLabel(s_lmap, LMAP_W - 96, 12 + 3 * 22, 0, F14, C_FLAG);
  lv_label_set_text(s_lmapFlagKey, "YELLOW");
  show(s_lmapFlagKey, false);
  for (int c = 0; c < F1_MAX_CORNERS; ++c) {
    s_lmapCorner[c] = mkLabel(s_lmap, 0, 0, 0, F14, C_DIM);
    show(s_lmapCorner[c], false);
  }
  s_lmapSF = mkBox(s_lmap, 0, 0, 6, 18, C_TEXT, 1);
  s_lmapNone = mkLabel(s_lmap, 0, 0, 0, F20, C_DIM);
  lv_label_set_text(s_lmapNone, "No outline for this circuit yet");
  lv_obj_center(s_lmapNone);
  for (int i = 0; i < F1_MAX_DRIVERS; ++i) {
    s_dot[i] = mkBox(s_lmap, 0, 0, 16, 16, 0x808080, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(s_dot[i], 2, 0);
    lv_obj_set_style_border_color(s_dot[i], lv_color_hex(C_BG), 0);
    s_dotLbl[i] = mkLabel(s_lmap, 0, 0, 0, F14, C_TEXT);
    show(s_dot[i], false);
    show(s_dotLbl[i], false);
  }
  lv_obj_t *wxBox = mkBox(s_pMap, LMAP_X, LMAP_H + 14, LMAP_W, 42, C_PANEL, 6);
  s_wx = mkLabel(wxBox, 16, 11, LMAP_W - 32, F16, C_TEXT);
  show(s_pMap, false);

  // Live: tyres. One object, drawn by drawTyres().
  s_pTyre = mkBox(s_table, 0, 62, SCR_W, 500, C_BG);
  s_tyChart = mkBox(s_pTyre, 0, 0, SCR_W, 500, C_BG);
  lv_obj_add_event_cb(s_tyChart, drawTyres, LV_EVENT_DRAW_MAIN, nullptr);
  show(s_pTyre, false);

  // Live: sectors. Same two columns as the tower.
  s_pSec = mkBox(s_table, 0, 62, SCR_W, 500, C_BG);
  for (int i = 0; i < ROWS; ++i) {
    SecRow &r = s_srow[i];
    r.box = mkBox(s_pSec, 12, 8 + i * ROW_H, COL_W - 4, ROW_H - 4, (i % 2) ? C_ROW : C_PANEL, 4);
    r.pos = mkLabel(r.box, 0, 7, 36, F24, C_DIM, LV_TEXT_ALIGN_RIGHT);
    r.bar = mkBox(r.box, 44, 5, 6, ROW_H - 14, 0x808080, 2);
    r.name = mkLabel(r.box, 58, 7, 70, F24, C_TEXT);
    for (int k = 0; k < 3; ++k) r.s[k] = mkLabel(r.box, 132 + k * 74, 5, 72, F20, C_YELLOW, LV_TEXT_ALIGN_RIGHT);
    r.lap = mkLabel(r.box, 356, 5, 92, F20, C_TEXT, LV_TEXT_ALIGN_RIGHT);
    r.trap = mkLabel(r.box, 452, 8, 38, F16, C_DIM, LV_TEXT_ALIGN_RIGHT);
    r.strip = lv_obj_create(r.box);
    lv_obj_remove_style_all(r.strip);
    lv_obj_set_pos(r.strip, 132, 31);
    lv_obj_set_size(r.strip, 222, 5);
    lv_obj_clear_flag(r.strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r.strip, drawSegs, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)i);
  }
  show(s_pSec, false);

  // Live: one car.
  s_pDrv = mkBox(s_table, 0, 62, SCR_W, 500, C_BG);
  lv_obj_t *frame = mkBox(s_pDrv, 24, 18, 100, 100, C_PANEL, 6);
  lv_obj_set_style_clip_corner(frame, true, 0);
  s_dImg = lv_img_create(frame);
  show(s_dImg, false);
  s_dBar = mkBox(s_pDrv, 24, 124, 100, 6, 0x808080, 3);
  s_dPos = mkLabel(s_pDrv, 138, 14, 104, F48, C_DIM);
  s_dCode = mkLabel(s_pDrv, 242, 14, 126, F48, C_TEXT);
  s_dName = mkLabel(s_pDrv, 138, 74, 228, F24, C_TEXT);
  s_dTeam = mkLabel(s_pDrv, 138, 106, 228, F20, C_DIM);
  s_dGap = mkLabel(s_pDrv, 24, 186, 320, F28, C_TEXT);
  lv_obj_set_style_text_line_space(s_dGap, 8, 0);
  s_dTyre = mkLabel(s_pDrv, 24, 286, 330, F24, C_TEXT);
  s_dPit = mkLabel(s_pDrv, 24, 336, 330, F16, C_DIM);
  lv_obj_set_style_text_line_space(s_dPit, 6, 0);

  auto caption = [](lv_obj_t *parent, int x, int y, const char *t) {
    lv_obj_t *l = mkLabel(parent, x, y, 0, F16, C_DIM);
    lv_label_set_text(l, t);
    return l;
  };
  auto bar = [](lv_obj_t *parent, int x, int y, int w, int h, uint32_t colour, int max) {
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(colour), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 4, LV_PART_INDICATOR);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_bar_set_range(b, 0, max);
    return b;
  };
  char spd[24];
  snprintf(spd, sizeof spd, "SPEED  %s", speedUnit());
  caption(s_pDrv, 372, 20, spd);
  s_dSpeed = mkLabel(s_pDrv, 372, 44, 190, F48, C_TEXT);
  caption(s_pDrv, 590, 20, "GEAR");
  s_dGear = mkLabel(s_pDrv, 590, 44, 90, F48, C_TEXT);
  caption(s_pDrv, 372, 128, "RPM");
  s_dRpm = mkLabel(s_pDrv, 572, 128, 110, F16, C_DIM, LV_TEXT_ALIGN_RIGHT);
  s_dRpmBar = bar(s_pDrv, 372, 152, 310, 20, 0x3B82F6, 13000);
  caption(s_pDrv, 372, 200, "THROTTLE");
  s_dThr = bar(s_pDrv, 372, 224, 310, 28, C_GREEN, 100);
  caption(s_pDrv, 372, 280, "BRAKE");
  s_dBrk = bar(s_pDrv, 372, 304, 310, 28, C_RED, 100);

  for (int k = 0; k < 3; ++k) {
    char n[4] = {'S', (char)('1' + k), 0, 0};
    lv_obj_t *c = mkLabel(s_pDrv, 724, 26 + k * 46, 0, F20, C_DIM);
    lv_label_set_text(c, n);
    s_dSec[k] = mkLabel(s_pDrv, 764, 20 + k * 46, 120, F28, C_TEXT);
    s_dSecBest[k] = mkLabel(s_pDrv, 884, 30 + k * 46, 116, F16, C_DIM, LV_TEXT_ALIGN_RIGHT);
  }
  caption(s_pDrv, 724, 186, "LAST LAP");
  s_dLast = mkLabel(s_pDrv, 840, 178, 160, F28, C_TEXT, LV_TEXT_ALIGN_RIGHT);
  caption(s_pDrv, 724, 232, "BEST LAP");
  s_dBest = mkLabel(s_pDrv, 840, 224, 160, F28, C_TEXT, LV_TEXT_ALIGN_RIGHT);
  caption(s_pDrv, 724, 278, "SPEED TRAP");
  s_dTrap = mkLabel(s_pDrv, 840, 270, 160, F28, C_TEXT, LV_TEXT_ALIGN_RIGHT);
  lv_obj_t *hint = mkLabel(s_pDrv, 0, 470, SCR_W, F14, C_DIM, LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(hint, LV_SYMBOL_LEFT "  car ahead               tap the middle to go back               car behind  " LV_SYMBOL_RIGHT);
  show(s_pDrv, false);

  // Countdown view
  s_next = mkBox(scr, 0, 0, SCR_W, SCR_H, C_BG);
  mkBox(s_next, 0, 0, SCR_W, 6, C_RED);
  s_nxKicker = mkLabel(s_next, 48, 40, 600, F20, C_RED);
  s_nxMeeting = mkLabel(s_next, 48, 72, 928, F48, C_TEXT);
  s_nxSession = mkLabel(s_next, 48, 136, 928, F28, C_DIM);
  s_nxCount = mkLabel(s_next, 48, 196, 600, F48, C_TEXT);
  lv_obj_set_style_text_letter_space(s_nxCount, 2, 0);
  mkBox(s_next, 48, 280, 928, 2, C_PANEL);
  s_nxTimes = mkLabel(s_next, 48, 304, 150, F24, C_DIM);
  s_nxNames = mkLabel(s_next, 210, 304, 700, F24, C_TEXT);
  lv_obj_set_style_text_line_space(s_nxTimes, 10, 0);
  lv_obj_set_style_text_line_space(s_nxNames, 10, 0);
  s_nxStatus = mkLabel(s_next, 48, SCR_H - 40, 928, F16, C_DIM);
  s_map = mkBox(s_next, SCR_W - 48 - MAP_W, 296, MAP_W, MAP_H + 28, C_BG);
  s_mapLine = lv_line_create(s_map);
  lv_obj_set_style_line_width(s_mapLine, 5, 0);
  lv_obj_set_style_line_color(s_mapLine, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_line_rounded(s_mapLine, true, 0);
  s_mapDot = mkBox(s_map, 0, 0, 12, 12, C_RED, LV_RADIUS_CIRCLE);
  s_mapName = mkLabel(s_map, 0, MAP_H + 6, MAP_W, F16, C_DIM, LV_TEXT_ALIGN_CENTER);
  show(s_map, false);

  // Boot view
  s_boot = mkBox(scr, 0, 0, SCR_W, SCR_H, C_BG);
  lv_obj_t *t = mkLabel(s_boot, 0, 0, 0, F48, C_TEXT);
  lv_label_set_text(t, "DARKSIDE F1");
  lv_obj_align(t, LV_ALIGN_CENTER, 0, -30);
  lv_obj_t *t2 = mkLabel(s_boot, 0, 0, 0, F20, C_DIM);
  lv_label_set_text(t2, "Starting");
  lv_obj_align(t2, LV_ALIGN_CENTER, 0, 30);

  // Full-screen alert and the overtake strip sit above everything.
  lv_obj_t *top = lv_layer_top();
  s_alert = mkBox(top, 0, 0, SCR_W, SCR_H, C_YELLOW);
  s_alertBig = mkLabel(s_alert, 0, 0, 0, F48, 0x000000);
  lv_obj_set_style_text_letter_space(s_alertBig, 4, 0);
  lv_obj_align(s_alertBig, LV_ALIGN_CENTER, 0, -60);
  s_alertSub = mkLabel(s_alert, 0, 0, 0, F40, 0x000000);
  lv_obj_align(s_alertSub, LV_ALIGN_CENTER, 0, 20);
  s_alertSub2 = mkLabel(s_alert, 0, 0, 0, F28, 0x000000);
  lv_obj_align(s_alertSub2, LV_ALIGN_CENTER, 0, 90);
  show(s_alert, false);

  s_toast = mkBox(top, 0, SCR_H - 38, SCR_W, 38, 0x000000);   // covers the footer only
  s_toastBar = mkBox(s_toast, 0, 0, 14, 38, C_RED);
  s_toastLbl = mkLabel(s_toast, 36, 5, SCR_W - 60, F24, C_TEXT);
  for (lv_obj_t *o : {s_toast, s_toastBar}) lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);   // taps reach the tabs below
  show(s_toast, false);

  show(s_table, false);
  show(s_next, false);
  s_viewSince = millis();
  lv_timer_create(tick, 500, nullptr);
  lv_timer_create(fastTick, 100, nullptr);
}
