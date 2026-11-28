// What a panel can be told after it has been flashed: Wi-Fi, the OpenF1
// login, the time zone and the units. The web flasher's set-up page sends
// them over USB serial and they are kept on the panel (NVS). Anything not
// stored comes from secrets.h / config.h, so a build with those filled in
// needs no set-up. Fixed from boot: a change is saved and the panel restarts.
//
// No Arduino headers here: ui.cpp includes this and is also built on a desktop.
#pragma once

struct Settings {
  char wifiSsid[33];    // empty = not set up
  char wifiPass[65];
  char f1User[128];     // empty = no OpenF1 login
  char f1Pass[128];
  char tz[48];          // POSIX TZ string
  bool fahrenheit;      // else Celsius
  bool mph;             // else km/h, and the wind in m/s
};

const Settings &settings();
void settingsLoad();    // call first in setup(), before anything reads settings()

// One line from the serial console that starts with 'K'. Values are hex, so
// any character gets through and nothing needs escaping:
//   K?            report (never a password): "[cfg] v1 fw=... ssid=<hex> login=0|1 tz=... units=FM|CK ..."
//   Kssid=<hex>   Wi-Fi name          Kwpass=<hex>  Wi-Fi password
//   Kuser=<hex>   OpenF1 username     Kupass=<hex>  OpenF1 password
//   Ktz=<hex>     POSIX time zone     Kunits=XY     X: F or C, Y: M (mph) or K (km/h)
//   K!            save what was sent and restart
//   K0            forget everything stored and restart
// Each is answered with one line: "[cfg] ok <key>", "[cfg] bad: <why>",
// "[cfg] saved - restarting" or "[cfg] cleared - restarting".
void settingsCommand(const char *line);
