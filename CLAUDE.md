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
  alert 3 (F1AlertKind; 9-12 are the footer strips). A screenshot fails if
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
- Status 2026-11-27: compiles (61% of 3 MB app). Not checkable over serial
  and still to be eyeballed on the glass: tearing, backlight level, the
  footer tabs under a finger.
