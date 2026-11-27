#!/usr/bin/env python3
"""Bench stand-in for OpenF1's MQTT broker.

    python3 tools/mqtt_bench.py --session 11420 --offset 200 --minutes 12

Downloads a window of a finished session over REST (cached on disk), then
serves it on port 1883 as the real broker would: one JSON object per message
on v1/<endpoint>, with _id and _key added. The panel, built with

    DEFINES="-DREPLAY_SESSION_KEY=11420 -DREPLAY_OFFSET_S=200 \
             -DMQTT_BENCH_HOST=<this Mac's LAN address>" ./build.sh flash

publishes its replay clock on bench/clock and rows are sent as that clock
passes their date, so the stream and the panel's REST backfill agree.

No dependencies: a minimal MQTT 3.1.1 server (CONNECT, SUBSCRIBE, PUBLISH
QoS 0, PINGREQ) is all the firmware needs. Lap and pit rows are sent the way
the live feed does it: first when they begin, again as sectors/durations
become known.
"""
import argparse, bisect, json, os, socket, sys, threading, time, urllib.error, urllib.parse, urllib.request
from datetime import datetime, timezone

API = "https://api.openf1.org/v1/"


def iso_ms(s):
    if not s:
        return 0
    s = s.replace("Z", "+00:00")
    d = datetime.fromisoformat(s)
    if d.tzinfo is None:
        d = d.replace(tzinfo=timezone.utc)
    return int(d.timestamp() * 1000)


def ms_iso(ms):
    return datetime.fromtimestamp(ms / 1000, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3]


_last_get = [0.0]


def get(cache, name, query):
    """One REST query -> list of rows, cached as JSON."""
    path = os.path.join(cache, name + ".json")
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    url = API + query.replace(" ", "%20")
    for attempt in range(6):
        wait = 2.2 - (time.time() - _last_get[0])          # stay under 30 requests a minute
        if wait > 0:
            time.sleep(wait)
        _last_get[0] = time.time()
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "bench"}), timeout=120) as r:
                rows = json.loads(r.read())
            break
        except urllib.error.HTTPError as e:
            if e.code == 404:
                rows = []
                break
            if e.code == 401:
                sys.exit("OpenF1 wants a login just now (a session is live): fill the cache when none is.")
            print(f"  HTTP {e.code} on {name}, retrying", flush=True)
            time.sleep(5 + 5 * attempt)
        except Exception as e:                              # noqa: BLE001
            print(f"  {e} on {name}, retrying", flush=True)
            time.sleep(5)
    else:
        sys.exit(f"could not fetch {query}")
    if not isinstance(rows, list):
        rows = []
    with open(path, "w") as f:
        json.dump(rows, f)
    print(f"  {name}: {len(rows)} rows", flush=True)
    return rows


def build_events(a):
    cache = os.path.join(a.cache, f"{a.session}_{a.offset}_{a.minutes}")
    os.makedirs(cache, exist_ok=True)
    K = a.session
    ses = get(cache, "session", f"sessions?session_key={K}")
    if not ses:
        sys.exit("no such session")
    start = iso_ms(ses[0]["date_start"])
    t0 = start + a.offset * 1000 - 15000
    t1 = start + (a.offset + a.minutes * 60) * 1000
    print(f"session {K} starts {ses[0]['date_start']}; window {ms_iso(t0)} .. {ms_iso(t1)}", flush=True)
    win = f"date>{ms_iso(t0)}&date<{ms_iso(t1)}"
    ev = []                                                 # (ms, order, topic, row)

    def add(t, topic, row, key):
        r = dict(row)
        r["_key"] = key
        ev.append((t, len(ev), topic, r))

    for ep in ("position", "intervals", "race_control", "weather", "overtakes", "team_radio"):
        for i, r in enumerate(get(cache, ep, f"{ep}?session_key={K}&{win}")):
            add(iso_ms(r.get("date")), "v1/" + ep, r, f"{ep}_{i}")

    laps = get(cache, "laps", f"laps?session_key={K}")
    lap_start = {}
    for r in laps:
        ts = iso_ms(r.get("date_start"))
        if not ts:
            continue
        lap_start[(r["driver_number"], r["lap_number"])] = ts
        if ts < t0 - 200000 or ts > t1:
            continue
        key = f"laps_{r['driver_number']}_{r['lap_number']}"
        late = ("lap_duration", "duration_sector_1", "duration_sector_2", "duration_sector_3",
                "segments_sector_1", "segments_sector_2", "segments_sector_3", "i1_speed", "i2_speed", "st_speed")
        blank = {k: None for k in late}
        s = [r.get("duration_sector_1"), r.get("duration_sector_2"), r.get("duration_sector_3")]
        add(ts, "v1/laps", {**r, **blank}, key)
        t = ts
        if s[0]:
            t += int(s[0] * 1000)
            add(t, "v1/laps", {**r, **blank, "duration_sector_1": s[0], "segments_sector_1": r.get("segments_sector_1"),
                               "i1_speed": r.get("i1_speed")}, key)
            if s[1]:
                t += int(s[1] * 1000)
                add(t, "v1/laps", {**r, "lap_duration": None, "duration_sector_3": None, "segments_sector_3": None,
                                   "st_speed": None}, key)
        if r.get("lap_duration"):
            add(ts + int(r["lap_duration"] * 1000) + 300, "v1/laps", r, key)

    for i, r in enumerate(get(cache, "pit", f"pit?session_key={K}")):
        ts = iso_ms(r.get("date"))
        if ts < t0 or ts > t1:
            continue
        key = f"pit_{i}"
        add(ts, "v1/pit", {**r, "lane_duration": None, "stop_duration": None, "pit_duration": None}, key)
        add(ts + int((r.get("lane_duration") or r.get("pit_duration") or 25) * 1000) + 500, "v1/pit", r, key)

    for i, r in enumerate(get(cache, "stints", f"stints?session_key={K}")):
        ts = lap_start.get((r["driver_number"], r.get("lap_start") or 1), 0)
        if (r.get("stint_number") or 1) == 1:
            ts = t0
        if not ts or ts < t0 or ts > t1:
            continue
        add(ts + 2000, "v1/stints", {**r, "lap_end": r.get("lap_start")}, f"stints_{i}")

    # The two heavy feeds, a minute at a time.
    for ep in ("location", "car_data"):
        n = 0
        for m in range(0, a.minutes + 1):
            c0 = t0 + m * 60000
            c1 = min(c0 + 60000, t1)
            if c0 >= t1:
                break
            rows = get(cache, f"{ep}_{m:02d}", f"{ep}?session_key={K}&date>={ms_iso(c0)}&date<{ms_iso(c1)}")
            for r in rows:
                add(iso_ms(r.get("date")), "v1/" + ep, r, f"{ep}_{n}")
                n += 1

    ev.sort(key=lambda e: (e[0], e[1]))
    out = []
    for i, (t, _, topic, r) in enumerate(ev):
        r["_id"] = i + 1
        out.append((t, topic.encode(), json.dumps(r, separators=(",", ":")).encode()))
    print(f"{len(out)} messages ready", flush=True)
    return out


# ---- MQTT ------------------------------------------------------------------

def varint(n):
    b = bytearray()
    while True:
        d = n % 128
        n //= 128
        b.append(d | (0x80 if n else 0))
        if not n:
            return bytes(b)


def publish_packet(topic, payload):
    body = len(topic).to_bytes(2, "big") + topic + payload
    return b"\x30" + varint(len(body)) + body


class Client(threading.Thread):
    def __init__(self, sock, addr, events):
        super().__init__(daemon=True)
        self.s, self.addr, self.ev = sock, addr, events
        self.times = [e[0] for e in events]
        self.subs = set()
        self.lock = threading.Lock()
        self.clock = None                                   # (data ms, monotonic s)
        self.at = None                                      # next event to send
        self.sent = 0
        self.alive = True

    def recv_exact(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise ConnectionError("closed")
            b += c
        return b

    def packet(self):
        h = self.recv_exact(1)[0]
        n, shift = 0, 0
        while True:
            d = self.recv_exact(1)[0]
            n |= (d & 0x7F) << shift
            shift += 7
            if not d & 0x80:
                break
        return h, self.recv_exact(n) if n else b""

    def send(self, data):
        with self.lock:
            self.s.sendall(data)

    def run(self):
        print(f"[{self.addr[0]}] connected", flush=True)
        threading.Thread(target=self.pump, daemon=True).start()
        try:
            while True:
                h, body = self.packet()
                t = h >> 4
                if t == 1:                                  # CONNECT
                    self.send(b"\x20\x02\x00\x00")
                elif t == 8:                                # SUBSCRIBE
                    pid, i, codes = body[:2], 2, b""
                    while i < len(body):
                        n = int.from_bytes(body[i:i + 2], "big")
                        self.subs.add(body[i + 2:i + 2 + n])
                        i += 2 + n + 1
                        codes += b"\x00"
                    self.send(b"\x90" + varint(2 + len(codes)) + pid + codes)
                elif t == 3:                                # PUBLISH (QoS 0)
                    n = int.from_bytes(body[:2], "big")
                    topic, payload = body[2:2 + n], body[2 + n:]
                    if topic == b"bench/clock":
                        self.on_clock(int(payload))
                elif t == 12:                               # PINGREQ
                    self.send(b"\xd0\x00")
                elif t == 14:                               # DISCONNECT
                    break
        except (ConnectionError, OSError, ValueError) as e:
            print(f"[{self.addr[0]}] {e}", flush=True)
        self.alive = False
        try:
            self.s.close()
        except OSError:
            pass
        print(f"[{self.addr[0]}] gone after {self.sent} messages", flush=True)

    def on_clock(self, ms):
        now = time.monotonic()
        if self.clock is None or abs(self.now_ms() - ms) > 5000:
            # First word from the panel, or its replay jumped: start from here.
            self.at = bisect.bisect_right(self.times, ms - 1500)
            print(f"[{self.addr[0]}] clock {ms_iso(ms)} -> message {self.at}/{len(self.ev)}", flush=True)
        self.clock = (ms, now)

    def now_ms(self):
        ms, at = self.clock
        return ms + int((time.monotonic() - at) * 1000)

    def pump(self):
        last_say = time.monotonic()
        while self.alive:
            time.sleep(0.05)
            if self.clock is None or self.at is None:
                continue
            now = self.now_ms()
            buf = bytearray()
            while self.at < len(self.ev) and self.ev[self.at][0] <= now:
                _, topic, payload = self.ev[self.at]
                self.at += 1
                if topic in self.subs:
                    buf += publish_packet(topic, payload)
                    self.sent += 1
            if buf:
                try:
                    self.send(bytes(buf))
                except OSError:
                    return
            if time.monotonic() - last_say > 30:
                last_say = time.monotonic()
                print(f"[{self.addr[0]}] {ms_iso(now)}  sent {self.sent}", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--session", type=int, default=11420)
    p.add_argument("--offset", type=int, default=200, help="seconds after the scheduled start (the firmware's REPLAY_OFFSET_S)")
    p.add_argument("--minutes", type=int, default=12)
    p.add_argument("--port", type=int, default=1883)
    p.add_argument("--cache", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench_cache"))
    p.add_argument("--fetch-only", action="store_true")
    a = p.parse_args()
    events = build_events(a)
    if a.fetch_only:
        return
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", a.port))
    srv.listen(4)
    print(f"MQTT bench broker on port {a.port}", flush=True)
    while True:
        s, addr = srv.accept()
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        Client(s, addr, events).start()


if __name__ == "__main__":
    main()
