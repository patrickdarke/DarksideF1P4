#!/usr/bin/env python3
"""Turn a sound file into start_sound.h, the clip the panel plays at a race start.

    python3 tools/make_sound.py ~/Downloads/lights.mp3

Needs ffmpeg. The clip is mixed down to mono at 16 kHz (the panel's speaker
is small), silence at both ends is cut off, the level is brought up to just
under full scale and the ends are faded over a few milliseconds so nothing
clicks. start_sound.h is not tracked: the firmware builds without it and is
then silent.
"""
import argparse
import os
import struct
import subprocess
import sys

RATE = 16000
QUIET = 0.02        # of the peak: below this at either end is silence
MARGIN_S = 0.01     # kept either side of the sound
FADE_S = 0.005
PEAK = 29000        # of 32767, about -1 dB
MAX_S = 12.0        # 16-bit mono at 16 kHz is 32 KB of flash a second


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("sound", help="any file ffmpeg can read")
    ap.add_argument("-o", "--out", default=os.path.join(here, "..", "start_sound.h"))
    a = ap.parse_args()

    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", a.sound, "-ac", "1", "-ar", str(RATE), "-f", "s16le", "-"],
        check=True, capture_output=True).stdout
    pcm = list(struct.unpack("<%dh" % (len(raw) // 2), raw[:len(raw) // 2 * 2]))
    peak = max((abs(v) for v in pcm), default=0)
    if peak < 50:
        sys.exit("that file is silent")

    loud = [i for i, v in enumerate(pcm) if abs(v) >= peak * QUIET]
    margin = int(MARGIN_S * RATE)
    pcm = pcm[max(0, loud[0] - margin):loud[-1] + 1 + margin]
    if len(pcm) > MAX_S * RATE:
        sys.exit("%.1f s of sound is too long for the panel (limit %.0f s)" % (len(pcm) / RATE, MAX_S))

    gain = PEAK / peak
    pcm = [int(round(v * gain)) for v in pcm]
    fade = min(int(FADE_S * RATE), len(pcm) // 2)
    for i in range(fade):
        pcm[i] = pcm[i] * i // fade
        pcm[-1 - i] = pcm[-1 - i] * i // fade

    with open(a.out, "w") as f:
        f.write("// Race-start sound, %d Hz mono. Made by tools/make_sound.py; not tracked.\n" % RATE)
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("#define START_SOUND_HZ %d\n" % RATE)
        f.write("static const int16_t kStartSound[%d] = {\n" % len(pcm))
        for i in range(0, len(pcm), 16):
            f.write(",".join(str(v) for v in pcm[i:i + 16]) + ",\n")
        f.write("};\n")
    print("%s: %.2f s, %d KB of flash, gain x%.1f" %
          (os.path.normpath(a.out), len(pcm) / RATE, len(pcm) * 2 // 1024, gain))


if __name__ == "__main__":
    main()
