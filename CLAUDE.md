# Darkside F1 P4 — working notes

Elecrow CrowPanel Advanced 10.1" ESP32-P4 (not the Waveshare 3.5" board of
DarksidePWRWS — different panel, touch and radio pins). Toolchain and
conventions follow that project: arduino-cli, core 3.3.11, `build.sh`,
`secrets.h` (never read it into context).

- Display/touch config files are Elecrow's Lesson09 (V1.0 tree, LVGL 8),
  unmodified: EK79007, 2-lane DSI, 51 MHz, GT911 on I2C0 (SDA 45/SCL 46),
  backlight PWM GPIO31. LDO3 2.5 V + LDO4 3.3 V must be acquired first.
- C6 SDIO pins differ from the core defaults: `WiFi.setPins(18,19,17,16,15,14,32)`.
- `f1_model.*` must stay free of Arduino/ESP headers: `tools/host_test.cc`
  builds it on a desktop and replays saved API responses.
- Live data comes two ways into the same model ingests, which are all safe
  to repeat: (1) the MQTT stream (`f1_mqtt.*`, `mqtts://mqtt.openf1.org:8883`,
  OAuth token as password, one JSON row per message on `v1/<endpoint>`),
  used when a login is configured and there is no replay / stream delay;
  (2) REST polls with `date>cursor` (replay and stream delay add
  `date<=dataNow`; empty results are HTTP 404). With the stream up REST only
  backfills at session start and re-reads slowly (`SYNC_*`, ~11 req/min) as
  a cross-check; 10 s after the stream drops the fast `POLL_*` periods
  return, including `location` / `car_data` for whatever screen is showing
  (`appWantFast`). The stream logs in again shortly before its token's hour
  is up and after `STREAM_QUIET_MS` of silence.
- MQTT TASK PRIORITY LAW (hardware, 2026-11-27): the esp-mqtt task must run
  at the network task's priority (4), not above it. At 5 (the LVGL task's
  priority) it boot-looped 7 times out of 7 within ~10 s of rows arriving:
  `Load access fault` in `vTaskSwitchContext`
  (prvSelectHighestPriorityTaskSMP, priority-5 ready list, a TCB whose list
  item had a null container). At 4: hours of bench stream, no fault. Only
  that one thing was changed, so the mechanism (priority inheritance on the
  model mutex is the suspect) is not proven. The MQTT task ingests under
  `appLock` itself only if the network task has not drained for 400 ms.
- Net task has a static (internal RAM) stack. UI reads the model under
  `appLock(0)` from an LVGL timer and skips a tick if it is busy.
- Circuit outline: `traceTrack()` in f1_app.cpp picks the fastest lap among
  `laps?lap_number=5` of a finished session at the circuit, then one
  `/location` query for that car and lap; `ingestTrack()` assumes the feed's
  y axis points up (flip it there if the map comes out mirrored). The same
  lap's sector times split the outline (`info.trackSec2/3` = first point of
  S2 / S3, 0 = unknown, then the map draws one grey loop).
- Corner numbers and marshal sectors: after the outline, `traceTrack()` gets
  `CIRCUIT_INFO_URL` (api.multiviewer.app, the file OpenF1's meetings name as
  `circuit_info_url`; ~18 KB, same coordinates as `/location`; fetched with
  `apiGetUrl`, which sends no login). `ingestCircuit()` puts corners and
  marshal-sector starts on the outline and drops the lot if any point is
  more than 5 % of the circuit's size away from it (another layout).
  Marshal sector k+1 = outline points `msecAt[k]`..`msecAt[k+1]`; the map
  shows that stretch while bit k of `live.yellowMask` is set (race control,
  scope "Sector"; a track-wide green / clear resets the mask). Desktop
  check on real data: Las Vegas 17 corners / 21 sectors, Lusail 16 / 20.
- Boot order law: `appInit()` (creates the model + mutex) must run before
  `uiBegin()`. The UI's LVGL timer calls `appLock()` within 500 ms; with a
  null mutex that asserts in xQueueSemaphoreTake and boot-loops (seen on
  hardware 2026-11-27).
- INTERNAL RAM LAW (hardware truth, measured 2026-11-27): the esp-hosted
  SDIO link to the C6 and the AES engine need DMA-capable internal RAM.
  With the core's defaults (malloc() prefers internal below 4 KB, mbedTLS
  always internal) only ~35 KB of it was left once TLS was up; an inbound
  burst used it up, any download above ~60 KB stalled part-way, and the C6
  link stayed dead (DNS and connect time-outs, Wi-Fi still "connected")
  until reboot. First sign in the log: `esp-aes: Failed to allocate memory`.
  Fix, first lines of `setup()`: `heap_caps_malloc_extmem_enable(0)` and
  `mbedtls_platform_set_calloc_free()` send malloc() and TLS to PSRAM.
  After it: 470 KB in 3 s, 3 MB in 25 s, DMA heap never below 130 KB.
  NOT the cause (tested): SDIO clock (20 MHz made no difference), the C6
  firmware version. Watch the `[sys] heap N dma (low M)` heartbeat; keep
  new big buffers out of internal RAM. `MALLOC_CAP_INTERNAL` free size
  misleads here: it counts a ~32 KB block that is not DMA-capable.
- C6: esp-hosted 2.12.3 on the co-processor, 2.12.11 on the host; works as
  is. Reset line is GPIO32 on this board (confirmed).
- Bench tools: `python3 tools/serial_hub.py DIR` owns the serial port
  (resets the board, logs to DIR/serial.log). `echo a.png > DIR/shot.req`
  pulls a screenshot of the real framebuffer over serial (firmware command
  "S"); `printf 'Gdrivers?session_key=latest' > DIR/cmd.req` runs one test
  GET in the net task and prints size, time and heap low-water. Other
  commands through cmd.req: `M` logs in to the real stream and reports
  (works with no session on), `V0`-`V4` live screen (tower, map, sectors,
  tyres, car), `D44` car 44's screen, `I1` between-session screen, `A3` shows
  alert 3 (F1AlertKind; 9-12 are the footer strips), `P` plays the race-start
  sound (changes nothing on the screen), `K?` reports the settings (never a
  password; the other `K` commands are in settings.h). A screenshot fails if
  any ESP-IDF log line lands inside it, which is why f1_mqtt.cpp silences
  the mqtt/tls log tags. One-off build settings without touching secrets.h:
  `DEFINES="-DREPLAY_SESSION_KEY=11420" ./build.sh flash`
  (`./build.sh <anything else>` just compiles WITHOUT the defines).
- Stream on the bench: `python3 tools/mqtt_bench.py --session 11420
  --offset 180 --minutes 14` downloads that window once (tools/bench_cache/)
  and serves it as a plain MQTT broker on :1883; build the panel with
  `-DREPLAY_SESSION_KEY=11420 -DREPLAY_OFFSET_S=180
  -DMQTT_BENCH_HOST=<Mac LAN address>` and it follows the panel's replay
  clock. Offset 2070 (4 min) is lap 19: safety car and 18 pit stops.
  `-DGRID_TEST_MEETING=1300` shows a finished meeting's starting grid;
  `-DSTREAM_QUIET_MS=20000` exercises the silent-stream reconnect.
- UI: the 500 ms tick redraws only what changed because `setColour`,
  `setBg`, `setFont` and `show` compare before setting (an LVGL style or
  flag set invalidates even when the value is the same). Keep that for
  anything on the map / car screens, which also refresh on a 100 ms timer
  when `live.fastVersion` moves. Units: `TEMP_FAHRENHEIT` and `SPEED_MPH` in
  config.h; the model keeps the feed's units (Celsius, km/h, m/s) and only
  ui.cpp converts (`temp()`, `speed()`, `wind()`).
- Live screens are picked with the footer tabs (`s_tabs`, hit zone = the
  footer row in `onTap`); header tap = next screen; a driver row opens the
  car screen and its middle goes back to `s_backView`. `-DLIVE_VIEW_START=1`
  boots on the map, `-DREPLAY_LOOP_S=800` restarts a replay after that many
  seconds (demo build).
- Tyre screen (`LV_TYRES`, `renderTyres` / `drawTyres`): every stint is kept
  per driver (`F1Driver::stint[]`, written over by stint number, future
  stints of a replay left out by the same lap test as the current
  compound). The chart is one object drawn in a draw callback from a copy
  (`s_tyRows`) and only invalidated when that copy changes.
- OWNER RULE (2026-11-27): no screen changes on its own. `IDLE_ROTATE_S` is 0
  (between-session screens change on a tap only) and nothing rotates the
  live screens; alerts and footer strips still come and go. The `V`/`D`/`I`
  debug commands change what he is looking at, so do not send them while he
  is watching the panel.
- Hardware 2026-11-27: EK79007 + GT911 bring-up confirmed on the owner's
  panel; no boot loop; Wi-Fi via the C6, NTP, OpenF1 paid token login,
  calendar, standings, results and the circuit outline (Lusail comes out
  the right way round) all confirmed; race replay 11420 runs.
- Hardware 2026-11-27 (evening), bench stream of race 11420 plus the real
  broker: MQTT tower, map with car dots, sectors with mini-sectors, car
  screen (telemetry, pit times, portrait), pit / radio / fastest-lap /
  overtake strips, full-screen safety-car alert, starting grid, Fahrenheit,
  fall-back to polling and automatic return to the stream all confirmed by
  screenshot. `M` against mqtt.openf1.org: login OK, 11 topics, 2.6-5.9 s,
  DMA heap low 141 KB. NOT yet seen: rows from the real broker (needs a
  live session; FP3 Sat 28 Nov 09:30 ET is the first).
- Hardware 2026-11-27 (late): sector-coloured outline (Las Vegas: 185
  points, S2 from 53, S3 from 117), mph on the car / sectors / weather
  displays and the footer tabs confirmed by screenshot. Touch works: the
  owner tapped between screens (before the tabs existed; the tab zones
  themselves are his to try). The plain build (no defines) went back on
  afterwards for Qatar FP3: boots, token, calendar, Lusail outline (330
  points, S2 from 124, S3 from 234), `M` login OK in 2.8 s, countdown
  screen unchanged after a minute (no rotation). The countdown screen's
  small outline is still one colour.
- Hardware 2026-11-27 (17:45), bench stream at `--offset 2070 --minutes 4`:
  corner numbers 1-17 and the yellow band for marshal sectors 17-18 (from
  the REST backfill), then sector 19 added when its row arrived on the
  stream; the panel fetched the circuit file itself (http 200). The owner
  tapped a driver row and got the car screen. OPEN: the bench stream
  dropped once (`[mqtt] disconnected`, the panel's side closed the socket,
  no `stopped` line) right after a portrait download and a screenshot;
  back up 3 s later on its own. Cause not found; not seen with screenshots
  alone. Plain build back on afterwards: Lusail info http 200, 16 corners,
  20 marshal sectors; `M` login OK in 3.8 s.
- Hardware 2026-11-27 (18:04): tyre screen confirmed by screenshot on the
  same bench stream (lap 19, then the new "M 1" stints after the
  safety-car stops arriving over the stream); five tabs fit the footer; no
  stream drop in that run.
- Hardware 2026-11-28 (06:30-06:55 ET), the review build below, three bench
  runs then the plain build left on for Qatar FP3:
  (a) bench stream of race 11420 at `--offset 2070` with
  `-DSTREAM_QUIET_MS=20000`: `[mqtt] caught up by REST` after the session
  start, after the broker was killed for 8 s, and after the silent-stream
  reconnect; tower and map by screenshot; no fault in ~6 min of stream.
  (b) REST replay of 11420 at `-DREPLAY_OFFSET_S=5400`: the flag falls ~80 s
  in and the replay restarted 195 s and 201 s after going live, i.e. 2 min
  after the flag both times (the old stamp fault restarted at once, half
  the time); chequered pill and final order by screenshot.
  (c) REST replay of qualifying 11416 at `-DREPLAY_OFFSET_S=1020`: chequered
  pill at the end of Q1, the row the old model dropped.
  Plain build: token, calendar, Lusail outline, `M` login OK in 2.7 s, DMA
  heap low 134 KB. Stack never used (`[sys] stack low`): lvgl 3.1-3.3 KB of
  6, f1net 16.4 KB of 20, mqtt 10.2 KB of 12, loop 5.8 KB.
  NOT yet seen on hardware: rows from the real broker, the hourly token
  renewal and stream re-login, the overrun rule and the late join.
  The 27 Nov binaries are kept in `logs/build_2026-11-27/` (not tracked).
- Review 2026-11-28 (what changed in behaviour; see the hardware note above
  for what has been confirmed on the panel):
  - Timestamps with 0 = "not set" come from `stampNow()` (stamp.h), never
    `millis() | 1`: that can be 1 ms ahead, and an unsigned "how long ago"
    in the same millisecond then reads 49.7 days. It closed the live screen
    the instant a race's flag fell (about every second race) and made the
    MQTT task drain at once half the time - so the "only after 400 ms" rule
    above is true from this build on, not before. A millis() deadline is
    compared signed and cleared once passed (`backingOff()`).
  - Stream hold (`holdStream` / `fillGap` in f1_app.cpp): from the start of
    every stream connection its rows are collected, not applied, and the
    feeds are polled fast, until one REST read of race control and position
    has been answered after the stream came up. Covers the session start
    and the 3-6 s of the hourly re-login. Log line: `[mqtt] caught up by
    REST`. `mqttDownMs()` no longer restarts at each reconnect attempt.
  - Race control: rows are not in date order (flag rows are cut to the
    second and sent after a status row stamped ~90 ms later). The model
    takes unseen rows up to `F1_RC_REORDER_MS` (5 s) behind the cursor and
    REST asks from that far back; before, every qualifying "CHEQUERED FLAG"
    and "RACE START" were dropped. Rows older than `F1_ALERT_FRESH_MS`
    change the state without a full-screen alert.
  - Token: a failed renewal keeps the current token and retries in 5 s; a
    live session is only left after `AUTH_REFUSED_MS` of refusals with the
    stream down. End of session: stays while race control has it under way,
    up to `LIVE_OVERRUN_MAX_S` past the timetable. Within that limit a
    session still going is also joined afresh after a reboot (`startLive`
    with `late`: drivers + race control are read, then `F1Model::progress()`
    decides; "over" is remembered, anything else is asked again every 2 min).
  - One request spacing timer for all requests (`pacedGet`); laps / pit
    history that failed at the session's start is asked for again (`s_owed`).
  - Heartbeat adds `[sys] stack low: lvgl N, f1net N, mqtt N, loop N`
    (bytes never used; the LVGL task has 6 KB in all - note the first values).
  - Desktop: `tools/model_test.cc` (unit tests, no samples) and
    `tools/host_test.cc`, which now uses the sessions' real keys and checks
    its result (it had been replaying nothing since the session filter went
    in: the model dropped every row as another session's).
- Race-start sound (`sound.*`, 2026-11-28): the full-screen start alert
  (`showAlert`, `ALERT_START`: races and sprints, live or replay, never a
  stale row) calls `soundRaceStart()`. Speaker = I2S straight into the
  onboard amplifier, no codec: BCLK 22, WS 21, DOUT 23, amplifier power
  GPIO30 active LOW (Elecrow's Lesson12 for this board; `SPK_*` in
  config.h; held off from boot by `soundBegin()`). The clip is
  `start_sound.h`, made by `python3 tools/make_sound.py <file>` (ffmpeg;
  16 kHz mono, silence trimmed, level normalised). It is the owner's own
  clip and is NOT tracked (.gitignore): without the file the build is
  silent. `SOUND_VOLUME` 0-100 (squared into the gain).
  Nothing exists between clips: each play makes a task (`f1snd`, priority 4
  like the network task - not higher, see the MQTT priority law - 6 KB
  stack) and an I2S channel with 8 x 512-frame DMA buffers (16 KB of
  internal DMA RAM, 256 ms, so a busy screen redraw cannot run it dry),
  then gives both back. The amplifier is powered only while the I2S clocks
  run, and one ring of silence is written after the clip before it goes off.
- Hardware 2026-11-28 (07:15-07:25 ET), sound: the owner heard the clip (five
  start-light beeps, 4.5 s) from the panel at volume 70 and asked for 20 %
  more: 84, played once more, and he said to keep it there. `P`
  twice and once from the real alert in a REST replay of race 11420 at `-DREPLAY_OFFSET_S=180` (lights out 46 s in):
  `[snd] race start played, 4821 ms, stack low 4116-4252` each time, no
  fault, DMA heap low 118-122 KB (it was ~122 KB before the first clip of
  that boot too: TLS, not the sound). App now 67% of 3 MB. NOT yet seen:
  the sound at a live race start with the stream busy (Sunday's race).
  The build before the sound is kept in `logs/build_2026-11-28_reviewed/`.
- Settings on the panel (`settings.*`, 2026-11-28): Wi-Fi name + password,
  OpenF1 username + password, POSIX time zone and units (F/C, mph/km/h) are
  read once at boot: NVS namespace `f1cfg` over the built-in values
  (secrets.h, `LOCAL_TZ`, `TEMP_FAHRENHEIT`, `SPEED_MPH`). The owner's panel
  has nothing stored, so it runs on secrets.h exactly as before. `K` lines
  on the serial console set them (hex values, one answer line each, `K!`
  saves and restarts, `K0` erases and restarts); nothing is changed while
  running. Units are therefore a run-time choice in ui.cpp (`inF()`,
  `inMph()`); the simulator's stub uses the owner's (18/18 screens
  unchanged). With no Wi-Fi name the boot screen shows `SETUP_URL` and
  Wi-Fi is not started. `Serial.setRxBufferSize(1024)`: a set-up line is up
  to 260 characters and loop() reads 5 times a second.
- Web flasher (`docs/`, GitHub Pages from `/docs` on main, switched on
  2026-11-28: a push to main republishes it within a minute or so; the live
  files were compared byte for byte with the committed ones that day):
  `docs/index.html` = ESP Web Tools 10.5.2 from unpkg (its manifest knows
  `ESP32-P4`; flashes at 115200 through the CH340) plus a set-up form that
  talks the `K` commands over Web Serial. `tools/make_release.sh` builds
  `docs/firmware/` (bootloader 0x2000, partitions 0x8000, boot_app0 0xe000,
  app 0x10000 + manifest.json, version = `FW_VERSION` in config.h) from a
  copy of only the files git tracks, with secrets.h.example as secrets.h:
  NEVER put a .bin there any other way - every other build has the owner's
  Wi-Fi and OpenF1 passwords inside it (.gitignore lets only
  `docs/firmware/*.bin` through). No start_sound.h in it either (silent),
  and `-ffile-prefix-map` keeps home-folder paths out; the script refuses
  to write if one is found. Run it again, and bump `FW_VERSION`, whenever
  the firmware changes. `tools/make_tzdata.py` writes `docs/tz.js` (IANA
  name -> POSIX string, from the Mac's zoneinfo).
- Hardware 2026-11-28 (07:40-07:50 ET), settings + release binary:
  (a) own build: boots on secrets.h (`stored=0`), `K?`; bad hex / unknown
  key / control character refused; `Ktz` + `Kunits` + `K!` -> restart, read
  back `stored=1`, clock in UTC by screenshot; a stored (made-up) Wi-Fi
  name wins over secrets.h (`net=0`); `K0` -> back on secrets.h, token and
  calendar fine. The software restart (`ESP.restart()`) brings the C6 link
  back each time.
  (b) the release files written with esptool at the manifest's four
  offsets: boots, `[cfg] Wi-Fi NOT SET`, set-up line on the boot screen by
  screenshot (docs/screens/setup.png), `K!` with no Wi-Fi name refused, `P`
  silent.
  (c) own build back on: token, calendar, `M` login OK in 3.2 s, sound
  plays. Sound while the stream's TLS session was up: DMA heap low 103 KB
  (the lowest seen today; trouble started near 35 KB).
  NOT tested by anyone yet: the page's Install button and its set-up form
  against a real panel (needs a person at Chrome's port chooser), and the
  release build on Wi-Fi (it has no credentials until the form gives it
  some). The page itself loads with no script errors (version from the
  manifest, 419 time zones).
- Status 2026-11-27: compiles (61% of 3 MB app). Not checkable over serial
  and still to be eyeballed on the glass: tearing, backlight level, the
  footer tabs under a finger.
