#!/usr/bin/env bash
# Builds what the web flasher installs (docs/firmware/): the firmware with no
# Wi-Fi name, no password and no sound clip in it, plus the manifest that
# ESP Web Tools reads. A panel flashed with it is given its Wi-Fi and login
# by the set-up page (docs/index.html) over USB.
#
#   tools/make_release.sh
#
# Nothing private can get in: the build is made from a copy of only the files
# git tracks (or would track), so everything .gitignore names - secrets.h,
# start_sound.h - is left behind, and the copy gets secrets.h.example as its
# secrets.h. Paths under your home folder are kept out of the binary as well,
# and the script stops if one is found in it.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HERE/docs/firmware"
FQBN="esp32:esp32:esp32p4:PSRAM=enabled,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=default,CDCOnBoot=default"
VERSION="$(sed -nE 's/^#define FW_VERSION "(.*)"/\1/p' "$HERE/config.h")"
[ -n "$VERSION" ] || { echo "✗ no FW_VERSION in config.h"; exit 1; }

WORK="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$WORK"' EXIT
SKETCH="$WORK/DarksideF1P4"
mkdir -p "$SKETCH"
(cd "$HERE" && git ls-files -z --cached --others --exclude-standard -- . ':!docs' ':!tools' |
  rsync -a --from0 --files-from=- . "$SKETCH/")
for private in secrets.h start_sound.h; do
  [ ! -e "$SKETCH/$private" ] || { echo "✗ $private is not ignored by git - not building a public binary from it"; exit 1; }
done
cp "$HERE/secrets.h.example" "$SKETCH/secrets.h"

# __FILE__ in the libraries would otherwise carry this machine's paths.
MAP="-ffile-prefix-map=$HOME=~ -ffile-prefix-map=/$HOME=~ -ffile-prefix-map=$WORK=."
arduino-cli compile --clean --fqbn "$FQBN" --libraries "$SKETCH/lib" --output-dir "$WORK/out" \
  --build-property "compiler.cpp.extra_flags=$MAP" --build-property "compiler.c.extra_flags=$MAP" "$SKETCH"

BOOT_APP0="$(ls "$(arduino-cli config get directories.data)"/packages/esp32/hardware/esp32/*/tools/partitions/boot_app0.bin | tail -1)"
mkdir -p "$OUT"
rm -f "$OUT"/*.bin "$OUT/manifest.json"
cp "$WORK/out/DarksideF1P4.ino.bootloader.bin" "$OUT/bootloader.bin"
cp "$WORK/out/DarksideF1P4.ino.partitions.bin" "$OUT/partitions.bin"
cp "$BOOT_APP0" "$OUT/boot_app0.bin"
cp "$WORK/out/DarksideF1P4.ino.bin" "$OUT/firmware.bin"

if strings -a "$OUT"/*.bin | grep -q -F "$HOME"; then
  rm -f "$OUT"/*.bin
  echo "✗ a path under $HOME got into the binary - nothing written"
  exit 1
fi

# Offsets as the Arduino core flashes an ESP32-P4: 0x2000, 0x8000, 0xe000, 0x10000.
cat > "$OUT/manifest.json" <<EOF
{
  "name": "Darkside F1 P4",
  "version": "$VERSION",
  "new_install_prompt_erase": true,
  "new_install_improv_wait_time": 0,
  "builds": [
    {
      "chipFamily": "ESP32-P4",
      "parts": [
        { "path": "bootloader.bin", "offset": 8192 },
        { "path": "partitions.bin", "offset": 32768 },
        { "path": "boot_app0.bin", "offset": 57344 },
        { "path": "firmware.bin", "offset": 65536 }
      ]
    }
  ]
}
EOF
echo "==> $OUT (version $VERSION)"
(cd "$OUT" && ls -l *.bin manifest.json | awk '{print "   ", $5, $9}' && shasum -a 256 firmware.bin | awk '{print "    sha256", $1}')
