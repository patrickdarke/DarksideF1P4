// Race-state model for the OpenF1 API. Portable C++ (ArduinoJson only, no
// Arduino/ESP headers) so tools/host_test.cc can run it on a desktop
// against saved API responses.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define F1_MAX_DRIVERS  24
#define F1_MAX_SESSIONS 170
#define F1_MAX_MEETINGS 34
#define F1_MAX_TEAMS    14
#define F1_ALERT_QUEUE  8
#define F1_MAX_TRACK    360   // points in a circuit outline
#define F1_MAX_STINTS   8     // sets of tyres kept per driver
#define F1_MAX_CORNERS  32
#define F1_MAX_MSEC     32    // marshal sectors (live.yellowMask has one bit each)
#define F1_MAX_SEGS     30    // mini-sectors in a lap (three sectors of up to ten)

enum F1Kind : uint8_t { F1_PRACTICE, F1_QUALI, F1_RACE };
enum F1Track : uint8_t { TRK_CLEAR, TRK_YELLOW, TRK_SC, TRK_VSC, TRK_RED, TRK_FINISHED };
enum F1AlertKind : uint8_t {
  ALERT_NONE, ALERT_START, ALERT_GREEN, ALERT_SC, ALERT_SC_ENDING, ALERT_VSC,
  ALERT_VSC_ENDING, ALERT_RED, ALERT_CHEQUERED,
  // Everything from here on is a footer strip, not a full-screen alert, and
  // is dropped rather than queued when the queue is full.
  ALERT_OVERTAKE, ALERT_PIT, ALERT_RADIO, ALERT_FASTEST
};
// Mini-sector colours as the timing feed codes them (2048 / 2049 / 2051 / 2064).
enum F1Seg : uint8_t { SEG_NONE, SEG_YELLOW, SEG_GREEN, SEG_PURPLE, SEG_PIT };

// ---- time helpers -------------------------------------------------------
int64_t f1ParseIso(const char *s);                      // -> epoch ms (UTC), 0 on failure
void    f1FmtIso(int64_t ms, char *out, size_t n);      // "YYYY-MM-DDTHH:MM:SS.mmm" (UTC)
void    f1FmtLap(float sec, char *out, size_t n);       // "1:34.151" / "58.204" / "" if <= 0

struct F1Who {            // identity of a driver in some session
  uint8_t  num;
  char     code[4];
  char     last[20];
  char     team[24];
  uint32_t colour;        // 0xRRGGBB
};

// One set of tyres. It ran from lapStart to the lap before the next stint's
// lapStart (the driver's current lap for the newest one). lapStart 0 = not known.
struct F1Stint { char compound; uint8_t age0; uint16_t lapStart; };

struct F1Driver {
  F1Who    who;
  uint8_t  pos;           // 0 = unknown
  int64_t  posMs;         // date of the newest position row
  uint8_t  gridPos;       // first position seen this session
  float    gap;           // to leader, s; < 0 = unknown
  float    interval;      // to car ahead, s; < 0 = unknown
  uint8_t  gapLaps;       // > 0 => "+N LAP" instead of gap
  uint8_t  intLaps;
  int64_t  intMs;         // date of the newest interval row
  float    lastLap;       // s, 0 = none
  float    best[4];       // best lap per phase (0 = practice/race, 1-3 = Q1-Q3)
  uint16_t lapNo;         // lap the driver is on (highest lap_number started)
  char     compound;      // S M H I W, '?' unknown
  uint8_t  tyreAge;       // laps on the current set
  uint8_t  stops;         // stint number - 1
  uint16_t stintLap;      // lap the current stint began on
  uint8_t  stintAge0;     // age of that set when it went on
  uint8_t  nStint;        // every stint so far, by stint number - 1
  F1Stint  stint[F1_MAX_STINTS];
  int64_t  pitMs;         // date of the newest pit row
  float    pitLane, pitStop;   // that stop: time in the pit lane / stationary, s; 0 = not known
  uint16_t pitLap;
  int64_t  pitToldMs;     // pit row already announced
  // Newest lap that has any sector time yet (the lap in progress, usually).
  uint16_t secLap;
  float    sector[3];     // s, 0 = not set yet
  float    bestSector[3]; // personal bests this session
  uint8_t  seg[F1_MAX_SEGS];   // F1Seg per mini-sector of that lap (not sent in races)
  uint8_t  nSeg[3];       // how many of them belong to each sector
  uint16_t trap, bestTrap;     // speed-trap km/h: newest lap that has one / personal best
  // Live telemetry (stream only; 0 date = never seen).
  int32_t  x, y;          // car position in the feed's own units (1/10 m)
  int64_t  locMs;
  int16_t  speed;         // km/h
  uint16_t rpm;
  int8_t   gear;
  uint8_t  throttle, brake;    // %
  int64_t  carMs;
  char     headUrl[184];       // portrait (small PNG), "" if none
};

struct F1Alert { uint8_t kind, a, b, pos; };   // a/b = driver numbers

struct F1Live {
  int      sessionKey;
  F1Kind   kind;
  bool     started, finished;
  F1Track  track;
  bool     scEnding;
  uint32_t yellowMask;    // sectors under local yellow
  uint8_t  qPhase;        // 0 none, 1-3
  int64_t  phaseStart[4];
  uint16_t lap;           // leader's lap
  char     note[112];     // newest race-control message worth showing
  float    airT, trackT;  // -100 = unknown
  bool     rain;
  float    humidity, pressure, windSpeed;   // %, mbar, m/s; < 0 = unknown
  int16_t  windDir;       // degrees the wind comes from, -1 = unknown
  float    bestSector[3]; // session bests, 0 = none
  uint16_t bestTrap;
  float    fastest;       // session's fastest lap, s
  uint8_t  fastestNum;
  uint8_t  nDrv;
  F1Driver drv[F1_MAX_DRIVERS];
  uint8_t  order[F1_MAX_DRIVERS];   // indices into drv[], running order
  int64_t  newestIntMs;
  uint32_t version;       // bumps on every change
  uint32_t fastVersion;   // bumps on car positions / telemetry only
};

struct F1SessionInfo {
  int     key, meetingKey, circuitKey;
  F1Kind  kind;
  char    name[24];       // "Practice 1", "Sprint Qualifying", "Race", ...
  int64_t startMs, endMs;
  bool    cancelled;
};
struct F1MeetingInfo {
  int  key;
  char name[40];          // "Qatar Grand Prix"
  char circuit[28];
  char country[28];
};
struct F1StandRow { uint8_t num; uint8_t pos, posStart; float pts, ptsStart; char team[24]; };
struct F1ResultRow {
  uint8_t num, pos;       // pos 0 = not classified
  bool    dnf, dns, dsq;
  float   time;           // race: total s for P1; quali/practice: best lap
  float   gap;            // s behind P1, < 0 unknown
  uint8_t gapLaps;
  uint16_t laps;
};
struct F1GridRow { uint8_t num, pos; float lap; };   // lap = the qualifying time, 0 = none

// Everything shown between sessions.
struct F1Info {
  int nSessions, nMeetings;
  F1SessionInfo sessions[F1_MAX_SESSIONS];   // sorted by start
  F1MeetingInfo meetings[F1_MAX_MEETINGS];
  int nWho;  F1Who who[F1_MAX_DRIVERS + 6];  // identities for standings/result
  int nDrvStand;  F1StandRow drvStand[F1_MAX_DRIVERS + 6];
  int nTeamStand; F1StandRow teamStand[F1_MAX_TEAMS];
  int standKey;                              // session the standings are from
  int resultKey; int nResult; F1ResultRow result[F1_MAX_DRIVERS];
  // Circuit outline traced from one car's lap. Screen-style coordinates
  // (y grows downward), scaled so the longer side spans 0..1000. Point 0 is
  // the start/finish line.
  int trackCircuit; int nTrack; int16_t trackX[F1_MAX_TRACK], trackY[F1_MAX_TRACK];
  int16_t trackW, trackH;                    // extent of the outline (one of them is 1000)
  int16_t trackSec2, trackSec3;              // first outline point of sectors 2 and 3 (0 = not known)
  // Feed units -> outline: X = (x - trackOX) * 1000 / trackSpan, Y = (trackOY - y) * 1000 / trackSpan.
  int32_t trackOX, trackOY, trackSpan;
  // From the circuit-info file, placed on that outline; both 0 until known.
  // A corner's number belongs at (cornerX, cornerY) in outline units, pushed
  // off the track along (cornerDX, cornerDY) / 100.
  uint8_t nCorner; uint8_t cornerNum[F1_MAX_CORNERS];
  int16_t cornerX[F1_MAX_CORNERS], cornerY[F1_MAX_CORNERS];
  int8_t cornerDX[F1_MAX_CORNERS], cornerDY[F1_MAX_CORNERS];
  // Marshal sector k+1 runs from outline point msecAt[k] to msecAt[k+1]
  // (the last one on to msecAt[0]).
  uint8_t nMsec; int16_t msecAt[F1_MAX_MSEC];
  int gridMeeting; int nGrid; F1GridRow grid[F1_MAX_DRIVERS];   // starting grid of that meeting
  uint32_t version;
};

class F1Model {
public:
  F1Live live;
  F1Info info;
  // Feed cursors (epoch ms): the newest row applied, used to ask REST for
  // "everything after". The feeds also arrive as a stream (MQTT), one row at
  // a time and possibly twice, so every ingest is safe to repeat.
  int64_t curPos, curInt, curRc, curPit, curOvt, curWx, curRadio, curLoc;
  bool    alertsArmed;
  // True when rows describe the present (no replay / stream delay): a stint
  // or lap that is about to start may then be applied as soon as it is sent.
  bool    liveNow;

  F1Model();
  void resetLive(int sessionKey, F1Kind kind);

  // Each ingest takes one API response body. Returns rows applied, 0 for an
  // empty/"No results" body, -1 if the JSON would not parse.
  int ingestDrivers(const char *json, size_t n);
  int ingestPosition(const char *json, size_t n);
  int ingestIntervals(const char *json, size_t n);
  int ingestLaps(const char *json, size_t n, int64_t nowMs);
  int ingestStints(const char *json, size_t n);
  int ingestRaceControl(const char *json, size_t n);
  int ingestPit(const char *json, size_t n);
  int ingestOvertakes(const char *json, size_t n);
  void flushOvertakes(int64_t nowMs);   // call after each poll round
  int ingestWeather(const char *json, size_t n);
  int ingestLocation(const char *json, size_t n);
  int ingestCarData(const char *json, size_t n);
  int ingestTeamRadio(const char *json, size_t n);

  int ingestSessions(const char *json, size_t n);
  int ingestMeetings(const char *json, size_t n);
  int ingestWho(const char *json, size_t n);
  int ingestDriverStandings(const char *json, size_t n, int sessionKey);
  int ingestTeamStandings(const char *json, size_t n);
  int ingestResult(const char *json, size_t n, int sessionKey);
  int ingestGrid(const char *json, size_t n, int meetingKey);
  // /location rows for one car over one lap -> info.track*. Returns points kept.
  // With the lap's start and its first two sector times, the outline is also
  // split into the three sectors.
  int ingestTrack(const char *json, size_t n, int circuitKey, int64_t lapStartMs = 0,
                  float sector1 = 0, float sector2 = 0);
  // The circuit-info file (corners, marshal sectors) for the circuit whose
  // outline is loaded. Returns corners kept; 0 if it does not fit the outline.
  int ingestCircuit(const char *json, size_t n, int circuitKey);
  // From /laps rows, pick the fastest clean lap. False if there is none.
  static bool pickLap(const char *json, size_t n, int *driver, int64_t *startMs, float *durS,
                      float *sector1 = nullptr, float *sector2 = nullptr);

  bool popAlert(F1Alert &out);
  F1Driver *driver(uint8_t num);
  const F1Who *who(uint8_t num) const;
  const F1SessionInfo *sessionByKey(int key) const;
  const F1MeetingInfo *meetingByKey(int key) const;
  // Session whose [start - leadMs, end + tailMs] window contains nowMs.
  const F1SessionInfo *activeSession(int64_t nowMs, int64_t leadMs, int64_t tailMs) const;
  const F1SessionInfo *nextSession(int64_t nowMs) const;
  const F1SessionInfo *lastEnded(int64_t nowMs, bool raceOnly) const;
  // Time shown for a driver in practice/qualifying; dim = from an earlier phase.
  float shownBest(const F1Driver &d, bool *dim) const;

private:
  F1Alert alerts_[F1_ALERT_QUEUE];
  uint8_t aHead_, aCount_;
  void pushAlert(uint8_t kind, uint8_t a = 0, uint8_t b = 0, uint8_t pos = 0);
  struct PendingOvt { int64_t t; uint8_t a, b, pos; };
  PendingOvt pend_[16];
  int nPend_;
  // (date, hash) of the newest rows of a feed whose rows can share a date.
  struct Seen { int64_t t[16]; uint32_t h[16]; int at; bool had(int64_t tt, uint32_t hh); };
  Seen seenRc_, seenOvt_;
  void sortOrder();
  uint8_t phaseOf(int64_t ms) const;
};
