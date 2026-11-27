#!/usr/bin/env bash
# Download the OpenF1 responses host_test.cc and sim/ replay (2026 Las Vegas
# race 11420 + qualifying 11416) into tools/samples/. Free API; returns 401
# while any F1 session is live unless OPENF1_TOKEN is set.
set -euo pipefail
cd "$(dirname "$0")"; mkdir -p samples out; cd samples
H=(); [ -n "${OPENF1_TOKEN:-}" ] && H=(-H "Authorization: Bearer $OPENF1_TOKEN")
g(){ curl -fsS -m 90 ${H[@]+"${H[@]}"} "https://api.openf1.org/v1/$1" -o "$2"; sleep 1.1; }   # empty array + set -u on macOS bash 3.2
g "sessions?year=2026" sessions2026.json
g "meetings?year=2026" meetings2026.json
for e in drivers position intervals laps stints race_control pit weather session_result overtakes championship_drivers championship_teams; do g "$e?session_key=11420" race_$e.json; done
for e in drivers laps stints race_control session_result position pit weather overtakes intervals; do g "$e?session_key=11416" quali_$e.json || echo "[]" > quali_$e.json; done
echo "samples in $(pwd)"
