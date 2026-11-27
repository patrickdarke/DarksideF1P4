#!/usr/bin/env python3
"""Hold the panel's serial port open: log everything, take screenshots on demand.

    python3 tools/serial_hub.py DIR [port]

Only one process can hold the port, so this one keeps it open; it resets
the board when it starts. Lines go to DIR/serial.log. To take a screenshot, create
DIR/shot.req containing an output filename (e.g. `echo a.png > DIR/shot.req`);
the PNG is written to DIR/<name> and the request file is removed.
DIR/cmd.req: its contents are sent to the board as a command line.
DIR/reset.req: resets the board.
Needs pyserial and Pillow.
"""
import glob, os, struct, sys, time
import serial
from PIL import Image

d = sys.argv[1]
port = sys.argv[2] if len(sys.argv) > 2 else (glob.glob("/dev/cu.wchusbserial*") + glob.glob("/dev/ttyUSB*"))[0]
os.makedirs(d, exist_ok=True)
log = open(os.path.join(d, "serial.log"), "ab", buffering=0)
s = serial.Serial(port, 115200, timeout=0.2)

def reset():
    s.dtr = False; s.rts = True; time.sleep(0.2); s.rts = False
    log.write(b"[hub] reset\n")

reset()                                     # start from a clean boot so the log is complete
req = os.path.join(d, "shot.req")
cmd = os.path.join(d, "cmd.req")
rst = os.path.join(d, "reset.req")
buf = b""

def decode(body, w, h, out):
    px = bytearray()
    for k, c in struct.iter_unpack("<HH", body):
        r = (c >> 11) << 3; g = ((c >> 5) & 0x3F) << 2; b = (c & 0x1F) << 3
        px += bytes((r | r >> 5, g | g >> 6, b | b >> 5)) * k
    Image.frombytes("RGB", (w, h), bytes(px)).save(out)

pending = None
while True:
    if pending is None and os.path.exists(req):
        name = open(req).read().strip() or "shot.png"
        os.remove(req)
        pending = (os.path.join(d, os.path.basename(name)), time.time())
        s.write(b"\nS\n")                   # leading newline clears line noise
    if os.path.exists(cmd):                 # raw line(s) to the board, e.g. "Gdrivers?session_key=latest"
        data = b"\n" + open(cmd, "rb").read().strip() + b"\n"   # leading newline clears line noise
        os.remove(cmd)
        s.write(data)
    if os.path.exists(rst):                 # hardware reset via the CH340's RTS line
        os.remove(rst)
        reset()
    buf += s.read(65536)
    i = buf.find(b"\nSHOT ")
    if i >= 0:
        log.write(buf[:i]); buf = buf[i:]      # keep "\nSHOT ..." until the frame is whole
        j = buf.find(b"\n", 1)
        if j < 0: continue
        _, w, h, ln, sm = buf[1:j].split()
        w, h, ln, sm = int(w), int(h), int(ln), int(sm)
        if len(buf) < j + 1 + ln + 9: continue
        body = buf[j + 1:j + 1 + ln]; buf = buf[j + 1 + ln + 9:]
        out = pending[0] if pending else os.path.join(d, "shot.png")
        if sum(body) & 0xFFFFFFFF == sm:
            decode(body, w, h, out)
            log.write(f"[hub] screenshot -> {out}\n".encode())
        else:
            log.write(b"[hub] screenshot checksum mismatch\n")
        pending = None
        continue
    if pending and time.time() - pending[1] > 120:
        log.write(b"[hub] screenshot timed out\n"); pending = None
    # Flush whole lines, but keep a tail that could still become "\nSHOT ".
    k = buf.rfind(b"\n")
    if k >= 0 and not b"\nSHOT "[:len(buf) - k].startswith(buf[k:k + 6]):
        log.write(buf[:k + 1]); buf = buf[k + 1:]
    elif k > 0:
        log.write(buf[:k]); buf = buf[k:]
