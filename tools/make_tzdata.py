#!/usr/bin/env python3
"""Writes docs/tz.js: IANA time zone name -> POSIX TZ string, for the set-up page.

The panel's C library wants the POSIX form ("EST5EDT,M3.2.0,M11.1.0"); a
browser knows the IANA name ("America/New_York"). Every compiled zone file
ends with its POSIX string, so this reads them from the system's zoneinfo.
Run it again when the rules of a zone you care about change.
"""
import json
import os

ZONEINFO = "/usr/share/zoneinfo"
HERE = os.path.dirname(os.path.abspath(__file__))


def posix(name):
    with open(os.path.join(ZONEINFO, name), "rb") as f:
        data = f.read()
    if not data.startswith(b"TZif") or not data.endswith(b"\n"):
        return None
    tail = data[:-1].rsplit(b"\n", 1)[-1].decode("ascii", "replace")
    ok = tail and len(tail) < 48 and all(33 <= ord(c) < 127 for c in tail)
    return tail if ok else None


names = {"UTC"}
for tab in ("zone1970.tab", "zone.tab"):
    path = os.path.join(ZONEINFO, tab)
    if os.path.exists(path):
        for line in open(path):
            if line.strip() and not line.startswith("#"):
                names.add(line.split("\t")[2].strip())

zones = {n: posix(n) for n in sorted(names) if os.path.exists(os.path.join(ZONEINFO, n))}
zones = {n: p for n, p in zones.items() if p}
out = os.path.join(HERE, "..", "docs", "tz.js")
with open(out, "w") as f:
    f.write("// IANA zone -> POSIX TZ string. Made by tools/make_tzdata.py from the system's zoneinfo.\n")
    f.write("window.TZ_POSIX = " + json.dumps(zones, separators=(",", ":"), sort_keys=True) + ";\n")
print("%s: %d zones" % (os.path.normpath(out), len(zones)))
