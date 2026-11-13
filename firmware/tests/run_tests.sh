#!/bin/sh
# Host tests for the firmware's pure logic (no ESP-IDF needed). Also checks parse_range against
# the Python prototype so the two stay in step.
set -e
cd "$(dirname "$0")"
cc -std=c11 -Wall -Wextra -Werror -o /tmp/metermind_test_proto test_proto.c ../main/proto.c -lm
/tmp/metermind_test_proto > /tmp/metermind_test_proto.out || { cat /tmp/metermind_test_proto.out; exit 1; }
tail -1 /tmp/metermind_test_proto.out
cd ../..
python3 - <<'PY'
import sys
sys.path.insert(0, ".")
import importlib.util, types
# meter.py imports pyserial; stub it so this runs without the venv.
for m in ("serial", "serial.tools", "serial.tools.list_ports"):
    sys.modules.setdefault(m, types.ModuleType(m))
sys.modules["serial"].Serial = object
sys.modules["serial"].SerialException = Exception
sys.modules["serial.tools.list_ports"].comports = lambda: []
sys.modules["serial.tools"].list_ports = sys.modules["serial.tools.list_ports"]
from bridge.meter import parse_range
lines = open("/tmp/metermind_test_proto.out").read().splitlines()
bad = 0
for i, l in enumerate(lines):
    if l.startswith("range "):
        label, c = l[6:].rsplit("|", 1)
        py = parse_range(label)
        if c == "None":
            ok = py is None
        else:
            ok = py is not None and abs(py - float(lines[i + 1][6:])) <= abs(py) * 1e-9
        if not ok:
            bad += 1
            print("MISMATCH with Python:", repr(label), py, lines[i + 1] if c != "None" else None)
print("python parity:", "OK" if not bad else f"{bad} mismatches")
sys.exit(1 if bad else 0)
PY
