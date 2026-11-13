#!/bin/sh
# Build the firmware and assemble the web flasher in flasher/ (GitHub Pages serves that folder).
#
#   . ~/esp/esp-idf-v5.5/export.sh && tools/build_flasher.sh
#
# The release build uses only firmware/sdkconfig.defaults (its own build folder and sdkconfig), so
# what the flasher ships does not depend on anyone's local menuconfig. It writes
#   flasher/manifest.json                       ESP Web Tools manifest (version from the app)
#   flasher/firmware/*.bin                      bootloader, partition table, OTA data, app
#   flasher/firmware/metermind-<ver>-full.bin   one merged image for esptool (write at 0x0)
#   flasher/meter-and-boards.jpg                the photo, from docs/
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
B="$ROOT/firmware/build-release"

rm -f "$B/sdkconfig"   # regenerate from sdkconfig.defaults every time (a stale one keeps old values)
idf.py -C firmware -B "$B" -D SDKCONFIG="$B/sdkconfig" build

OUT="$ROOT/flasher/firmware"
rm -rf "$OUT"
mkdir -p "$OUT"
python3 - "$B" "$ROOT/flasher" <<'PY'
import json, os, shutil, sys
build, flasher = sys.argv[1], sys.argv[2]
args = json.load(open(os.path.join(build, "flasher_args.json")))
version = json.load(open(os.path.join(build, "project_description.json")))["project_version"]
parts = []
for offset, path in sorted(args["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
    name = os.path.basename(path)
    shutil.copy(os.path.join(build, path), os.path.join(flasher, "firmware", name))
    parts.append({"path": "firmware/" + name, "offset": int(offset, 16)})
manifest = {
    "name": "MeterMind",
    "version": version,
    "home_assistant_domain": None,
    "new_install_prompt_erase": True,
    "new_install_improv_wait_time": 0,  # the firmware does not speak Improv; WiFi setup is its own page
    "builds": [{"chipFamily": "ESP32-S3", "parts": parts}],
}
manifest = {k: v for k, v in manifest.items() if v is not None}
json.dump(manifest, open(os.path.join(flasher, "manifest.json"), "w"), indent=2)
open(os.path.join(build, "version.txt"), "w").write(version)
print(f"MeterMind {version}: " + ", ".join(f"{p['path']} @ {p['offset']:#x}" for p in parts))
PY
VER=$(cat "$B/version.txt")
cp "$ROOT/docs/meter-and-boards.jpg" "$ROOT/flasher/"
(cd "$B" && python -m esptool --chip esp32s3 merge_bin -o "$OUT/metermind-$VER-full.bin" @flash_args)
ls -l "$OUT"
