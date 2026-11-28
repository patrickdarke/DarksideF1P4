// Darkside F1 P4 — settings. Anything here can be overridden from
// secrets.h (gitignored), which is included first.
#pragma once
#include "secrets.h"

// ---- OpenF1 --------------------------------------------------------------
// Live timing needs a paid OpenF1 account (https://openf1.org). Leave the
// username empty to run on free data only: countdown, standings, results,
// and REPLAY mode all work; the live tower will not.
#ifndef OPENF1_USERNAME
#define OPENF1_USERNAME ""
#endif
#ifndef OPENF1_PASSWORD
#define OPENF1_PASSWORD ""
#endif
#define OPENF1_HOST "api.openf1.org"

// Live data is pushed over MQTT when an OpenF1 login is configured (the
// broker needs a paid account). REST polling stays for history, as a slow
// cross-check while the stream is up, as the fallback when it is not, and
// for replay and stream delay, which need data cut off at a point in time.
#ifndef USE_MQTT
#define USE_MQTT 1
#endif
// Bench: -DMQTT_BENCH_HOST=192.168.1.50 points the stream at tools/mqtt_bench.py
// on the Mac (plain TCP, port 1883), which replays a recorded session in
// step with REPLAY_SESSION_KEY.
#define F1_STR2(x) #x
#define F1_STR(x) F1_STR2(x)
#ifdef MQTT_BENCH_HOST
#define MQTT_URI "mqtt://" F1_STR(MQTT_BENCH_HOST) ":1883"
#else
#define MQTT_URI "mqtts://mqtt.openf1.org:8883"
#endif

// Replay a past session through the live screens (free data, no account).
// 0 = off. Example: 11420 = 2026 Las Vegas race.
#ifndef REPLAY_SESSION_KEY
#define REPLAY_SESSION_KEY 0
#endif
#ifndef REPLAY_OFFSET_S
#define REPLAY_OFFSET_S (-120)  // where to start, relative to the session's scheduled start
#endif
#define REPLAY_SPEED    1       // 1 = real time; 4 = four times faster
// Demo loop: start the replay again after this many seconds (0 = play it out).
#ifndef REPLAY_LOOP_S
#define REPLAY_LOOP_S 0
#endif
// Live screen shown first: 0 tower, 1 map, 2 sectors, 3 tyres, 4 one car.
#ifndef LIVE_VIEW_START
#define LIVE_VIEW_START 0
#endif

// Air and track temperature: 1 = Fahrenheit, 0 = Celsius (the feed's unit).
// Units and the time zone below are what a panel uses until the set-up page
// stores others on it (settings.h).
#define TEMP_FAHRENHEIT 1
#define SPEED_MPH       1       // car, speed-trap and wind speeds in mph (0 = km/h and m/s)

// Hold the display back to match a delayed TV stream (seconds, 0-120).
#ifndef STREAM_DELAY_S
#define STREAM_DELAY_S 0
#endif

// ---- timing --------------------------------------------------------------
#define LIVE_LEAD_S   (15 * 60)  // switch to the live screen this long before a session
#define LIVE_TAIL_S   (30 * 60)  // and stay this long after its scheduled end
// A delayed or red-flagged session runs past the timetable. The live screen
// stays while race control has it started and not finished and rows are
// still arriving, until it has been stopped for the grace time (qualifying
// is "finished" between its segments) or the limit is reached. Up to the
// same limit a session that is still going is also joined afresh (after a
// reboot); one that race control has over is not shown again.
#define LIVE_OVERRUN_GRACE_S (15 * 60)
#define LIVE_OVERRUN_MAX_S   (4 * 3600)
// OpenF1 allows 6 requests/second and 60/minute. Without the stream the
// rates below add up to 48/minute in a race (30 fast + 5 laps + 5 overtakes
// + 3 pit + 2 stints + 2 team radio + 1 weather), and to 60 with the map up
// (location, 12) or 63 with a car's screen (car_data, 15). API_MAX_PER_MIN
// is a hard cap enforced on every request, so on those two screens every
// feed then runs about a quarter slow.
#define API_MAX_PER_MIN 50
#define POLL_FAST_MS  6000       // position / intervals / race control
#define POLL_LAPS_MS  12000      // laps, overtakes
#define POLL_PIT_MS   20000
#define POLL_SLOW_MS  30000      // stints
#define POLL_WX_MS    60000
#define POLL_GAP_MS   350        // minimum spacing between any two requests (under 3/second)
// While the stream is up the same feeds are only re-read this often (ms):
// about 11 requests a minute in a race.
#define SYNC_FAST_MS  30000      // position / race control / laps
#define SYNC_SLOW_MS  60000      // intervals, stints
#define SYNC_PIT_MS   45000      // pit, overtakes
#define SYNC_WX_MS    120000
#ifndef STREAM_QUIET_MS
#define STREAM_QUIET_MS 180000   // stream up but silent this long: reconnect
#endif
// Stream down this long -> back to the fast polls. They come back sooner in
// practice: a reconnection holds the stream's rows (holdStream in f1_app.cpp)
// and the feeds are polled fast until REST has caught up.
#define STREAM_GRACE_MS 10000
#define AUTH_REFUSED_MS 20000    // every poll refused (401/403) this long -> the login is no good, leave the live screen
#define POLL_LOC_MS   5000       // car positions by REST, only without the stream and with the map on screen
#define POLL_CAR_MS   4000       // telemetry of the driver on screen, likewise
// Corner numbers and marshal sectors per circuit: the file OpenF1's meetings
// point to (circuit_info_url). Circuit key, then year.
#define CIRCUIT_INFO_URL "https://api.multiviewer.app/api/v1/circuits/%d/%d"
#define IDLE_ROTATE_S 0          // seconds per between-session screen; 0 = stay until tapped
#define ALERT_SHOW_MS 6000
#define TOAST_SHOW_MS 3500       // overtake strip
#define OVERTAKE_TOASTS 1        // 0 = no overtake strip

// ---- local time ----------------------------------------------------------
// POSIX TZ string. US Eastern shown; see
// https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
#ifndef LOCAL_TZ
#define LOCAL_TZ "EST5EDT,M3.2.0,M11.1.0"
#endif
#define NTP_SERVER "pool.ntp.org"

// ---- board ---------------------------------------------------------------
// malloc() calls above this many bytes go to PSRAM first (see the INTERNAL
// RAM LAW in DarksideF1P4.ino). The core's default is 4096.
#ifndef INTERNAL_MALLOC_MAX
#define INTERNAL_MALLOC_MAX 0
#endif
#define BACKLIGHT_PCT 80
#define MDNS_HOST "darksidef1"
// Shown by the set-up page, and in the manifest the web flasher reads (tools/make_release.sh).
#define FW_VERSION "2026.11.28"
// Where the web flasher and its set-up page live; shown on a panel with no Wi-Fi name.
#define SETUP_URL "patrickdarke.github.io/DarksideF1P4"
// ESP32-C6 radio over SDIO (esp-hosted). The Arduino core's defaults are
// for Espressif's own P4 board and are wrong here, so they are set
// explicitly. Elecrow's notes disagree on the C6 reset line for the V1.2
// board (32 in their code, 54 in a comment): if Wi-Fi never comes up on a
// V1.2 board, try 54.
#define C6_SDIO_CLK 18
#define C6_SDIO_CMD 19
#define C6_SDIO_D0  17
#define C6_SDIO_D1  16
#define C6_SDIO_D2  15
#define C6_SDIO_D3  14
#ifndef C6_RESET
#define C6_RESET    32
#endif
// Speaker: I2S straight into the onboard amplifier, no codec (pins from
// Elecrow's Lesson12 for this board). The amplifier's power pin is active LOW.
#define SPK_I2S_BCLK 22
#define SPK_I2S_WS   21
#define SPK_I2S_DOUT 23
#define SPK_AMP_EN   30
// Race-start sound (start_sound.h, made by tools/make_sound.py): 0-100, 0 = silent.
#ifndef SOUND_VOLUME
#define SOUND_VOLUME 84
#endif
