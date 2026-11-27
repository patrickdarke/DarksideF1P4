// Network task: clock, calendar, live polling, between-session data.
#pragma once
#include <stdint.h>

#include "f1_model.h"

enum AppMode : uint8_t { APP_BOOT, APP_IDLE, APP_LIVE };

struct AppStatus {
  AppMode mode;
  bool    wifi, timeOk, replay;
  bool    authNeeded;       // the API refused live data: no or bad OpenF1 login
  bool    stream;           // live rows are arriving over MQTT (else REST polling)
  int     liveKey;          // session on the live screen
  int64_t dataNowMs;        // "now" for the data shown (replay / stream delay)
  char    msg[72];          // one-line status for the boot and countdown screens
};

void appInit();    // model + lock; call BEFORE uiBegin (the UI timer takes the lock)
void appBegin();   // start the network task; call after Wi-Fi is configured
// The model and status are shared with the UI: hold the lock while reading.
bool appLock(uint32_t waitMs);
void appUnlock();
F1Model &appModel();
const AppStatus &appStatus();
int64_t appWallMs();        // UTC epoch ms, 0 until the clock is set
// The UI says what is on screen so that, without the stream, only the fast
// data that is actually shown gets polled: car positions (map) and one
// driver's telemetry (0 = none).
void appWantFast(bool positions, uint8_t telemetryDriver);
void appDebugFetch(const char *pathQuery);   // bench aid: one GET from the net task, result on serial
void appDebugStream();                       // bench aid: log in to the live stream now and report
