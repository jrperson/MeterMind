"""Meter core: owns the serial link and hides the XDM1041 quirks.

Structured the way the ESP32 firmware will be: one worker (a thread here, a FreeRTOS
task on the ESP32) owns the port, runs queued jobs, and polls the meter when idle.
Nothing else ever touches the serial port. See PROTOCOL.md for the quirks handled here.
"""

from __future__ import annotations

import math
import queue
import re
import threading
import time
from concurrent.futures import Future
from typing import Any, Callable

import serial
from serial.tools import list_ports

CH340_VID, CH340_PID = 0x1A86, 0x7523
BAUD = 115200

# name: (configure command, FUNC? reply, unit, supports range)
FUNCTIONS: dict[str, tuple[str, str, str, bool]] = {
    "dcv": ("CONF:VOLT:DC", "VOLT", "V", True),
    "acv": ("CONF:VOLT:AC", "VOLT AC", "V", True),
    "dci": ("CONF:CURR:DC", "CURR", "A", True),
    "aci": ("CONF:CURR:AC", "CURR AC", "A", True),
    "res": ("CONF:RES", "RES", "ohm", True),
    "cap": ("CONF:CAP", "CAP", "F", True),
    "freq": ("CONF:FREQ", "FREQ", "Hz", False),
    "period": ("CONF:PER", "PER", "s", False),
    "diode": ("CONF:DIOD", "DIOD", "V", False),
    "continuity": ("CONF:CONT", "CONT", "ohm", False),
    "temp": ("CONF:TEMP", "TEMP", "C", False),
}
FUNC_BY_REPLY = {v[1]: k for k, v in FUNCTIONS.items()}
# Full-scale ranges in SI units, in `RANGE <n>` index order (n = position + 1). Measured with
# a RANGE 1..9 sweep. The meter rejects CONF:<f> <value> unless value is exactly one of these.
RANGES: dict[str, list[float]] = {
    "dcv": [0.05, 0.5, 5, 50, 500, 1000],
    "acv": [0.5, 5, 50, 500, 750],
    "dci": [500e-6, 5e-3, 50e-3, 500e-3, 5, 10],
    "aci": [500e-6, 5e-3, 50e-3, 500e-3, 5, 10],
    "res": [500, 5e3, 50e3, 500e3, 5e6, 50e6],
    "cap": [50e-9, 500e-9, 5e-6, 50e-6, 500e-6, 5e-3, 50e-3],
}


def select_range(function: str, value: float) -> tuple[int, float]:
    """Smallest range that can show `value`. Returns (RANGE index, full-scale value)."""
    for i, r in enumerate(RANGES[function], start=1):
        if r >= value * (1 - 1e-9):
            return i, r
    raise MeterError("bad_request", f"{value:g} exceeds the largest {function} range "
                     f"({RANGES[function][-1]:g})", ranges=RANGES[function])
RATES = {"slow": "S", "medium": "M", "fast": "F"}
RATE_BY_CODE = {v: k for k, v in RATES.items()}
# Measured interval between new readings at each rate (PROTOCOL.md).
UPDATE_INTERVAL_S = {"slow": 1.0, "medium": 0.67, "fast": 0.33}

QUERY_TIMEOUT_S = 0.6     # unsupported commands never reply; 600 ms is enough
READY_PROBE_TIMEOUT_S = 0.08
POLL_PERIOD_S = 0.1       # MEAS?/RANGE?/MEAS:SHOW? polling
STATE_POLL_PERIOD_S = 1.0 # FUNC?/AUTO?/RATE? polling (front-panel changes)
# After a function or range change the meter reports stale values (the previous raw number
# rescaled into the new unit) for about 2 to 2.8 s. A reading counts as real once the value
# changes at least SETTLE_MIN_S after the change, or after SETTLE_MAX_S regardless.
SETTLE_MIN_S = 1.5
SETTLE_MAX_S = 3.5
OVERLOAD_THRESHOLD = 9.9e8  # meter reports 1E+9 for overload/open
MISSES_BEFORE_OFFLINE = 3

_PREFIX = {"n": 1e-9, "u": 1e-6, "µ": 1e-6, "m": 1e-3, "": 1.0, "k": 1e3, "K": 1e3, "M": 1e6, "G": 1e9}
_RANGE_RE = re.compile(r"^\s*([0-9]*\.?[0-9]+)\s*([numµkKMG]?)")


class MeterError(Exception):
    """Error with a stable code that maps onto the API's error codes."""

    def __init__(self, code: str, message: str, **extra: Any):
        super().__init__(message)
        self.code = code
        self.message = message
        self.extra = extra


def parse_range(label: str | None) -> float | None:
    """'500 mV' -> 0.5, '5uF' -> 5e-6, '50 MΩ' -> 5e7, 'Hz' / '' / 'KITS90' -> None."""
    if not label:
        return None
    m = _RANGE_RE.match(label)
    if not m:
        return None
    return float(m.group(1)) * _PREFIX[m.group(2)]


_NUMBER_ONLY = re.compile(r"^[-+0-9.Ee]+$")


def valid_range_label(label: str | None) -> bool:
    """RANGE? replies carry a unit ('5 V', 'Hz') or are empty. A bare number is a stray reply."""
    return label is not None and not _NUMBER_ONLY.match(label)


def valid_measurement(raw: str | None) -> bool:
    """MEAS? always answers in scientific notation ('4.58E-05', '1E+9')."""
    return raw is not None and "E" in raw and bool(_NUMBER_ONLY.match(raw))


def find_port() -> str | None:
    for p in list_ports.comports():
        if p.vid == CH340_VID and p.pid == CH340_PID:
            return p.device
    return None


class Meter:
    def __init__(self, port: str | None = None, on_event: Callable[[dict], None] | None = None):
        self.port_name = port
        self.on_event = on_event or (lambda e: None)
        self._jobs: queue.Queue[tuple[Callable[[], Any], Future]] = queue.Queue()
        self._ser: serial.Serial | None = None
        self._thread: threading.Thread | None = None
        self._running = False
        self._t0 = time.monotonic()

        self.port_open = False
        self.responding = False
        self.info: dict[str, Any] = {}
        self.state: dict[str, Any] = {
            "function": None, "range": None, "range_label": None, "autorange": None,
            "rate": None, "beep": None, "temp_unit": None, "settled": False,
        }
        self.seq = 0
        self.last_sample: dict[str, Any] | None = None

        self._misses = 0
        self._last_raw: str | None = None
        self._last_emit = 0.0
        self._settle_start: float | None = None
        self._next_state_poll = 0.0

    # ------------------------------------------------------------------ public API
    @property
    def connected(self) -> bool:
        return self.port_open and self.responding

    def start(self) -> None:
        self._running = True
        self._thread = threading.Thread(target=self._run, name="meter", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        if self._thread:
            self._thread.join(timeout=2)
        self._close()

    def millis(self) -> int:
        return int((time.monotonic() - self._t0) * 1000)

    def submit(self, fn: Callable[[], Any]) -> Future:
        fut: Future = Future()
        self._jobs.put((fn, fut))
        return fut

    def configure(self, function: str | None = None, range_: Any = None,
                  rate: str | None = None, beep: bool | None = None) -> Future:
        """Arguments must already be validated (see api.normalize_config)."""
        return self.submit(lambda: self._do_configure(function, range_, rate, beep))

    def scpi(self, cmd: str) -> Future:
        return self.submit(lambda: self._do_scpi(cmd))

    def snapshot(self) -> dict[str, Any]:
        return dict(self.state)

    # ------------------------------------------------------------------ worker
    def _run(self) -> None:
        next_poll = 0.0
        while self._running:
            if not self.port_open:
                self._fail_pending("meter_disconnected", "meter USB port is not open")
                if not self._open():
                    time.sleep(2.0)
                    continue
                next_poll = 0.0
            try:
                wait = max(0.0, next_poll - time.monotonic())
                try:
                    fn, fut = self._jobs.get(timeout=wait)
                except queue.Empty:
                    next_poll = time.monotonic() + POLL_PERIOD_S
                    self._poll()
                    continue
                if not fut.set_running_or_notify_cancel():
                    continue
                if not self.responding:
                    fut.set_exception(MeterError("meter_disconnected", "meter is not responding"))
                    continue
                try:
                    fut.set_result(fn())
                except serial.SerialException:
                    fut.set_exception(MeterError("meter_disconnected", "lost USB connection to meter"))
                    raise
                except MeterError as e:
                    fut.set_exception(e)
                except Exception as e:  # never kill the worker
                    fut.set_exception(MeterError("internal", f"{type(e).__name__}: {e}"))
            except (serial.SerialException, OSError):
                self._close()

    def _fail_pending(self, code: str, msg: str) -> None:
        while True:
            try:
                _, fut = self._jobs.get_nowait()
            except queue.Empty:
                return
            if fut.set_running_or_notify_cancel():
                fut.set_exception(MeterError(code, msg))

    def _open(self) -> bool:
        name = self.port_name or find_port()
        if not name:
            return False
        try:
            self._ser = serial.Serial(name, BAUD, timeout=QUERY_TIMEOUT_S)
        except (serial.SerialException, OSError):
            return False
        time.sleep(0.2)
        self._ser.reset_input_buffer()
        self.port_open = True
        self.info["port"] = name
        self._emit_status()
        self._on_responding()
        return True

    def _close(self) -> None:
        if self._ser:
            try:
                self._ser.close()
            except Exception:
                pass
        self._ser = None
        if self.port_open or self.responding:
            self.port_open = False
            self.responding = False
            self._emit_status()

    def _on_responding(self) -> None:
        """Meter (re)appeared: read identity and full state."""
        idn = self._query("*IDN?")
        if idn is None:
            self.responding = False
            return
        parts = [p.strip() for p in idn.split(",")]
        self.info.update({
            "manufacturer": parts[0] if parts else None,
            "model": parts[1] if len(parts) > 1 else None,
            "serial": parts[2] if len(parts) > 2 else None,
            "meter_fw": parts[3] if len(parts) > 3 else None,
            "idn": idn,
        })
        self.responding = True
        self._misses = 0
        self._refresh_state(full=True)
        self.state["settled"] = True
        self._emit_status()
        self._emit_state()

    # ------------------------------------------------------------------ serial primitives
    def _write(self, cmd: str) -> None:
        assert self._ser
        self._ser.write(cmd.encode("ascii") + b"\n")

    def _query_raw(self, cmd: str, timeout: float = QUERY_TIMEOUT_S) -> bytes | None:
        assert self._ser
        self._ser.reset_input_buffer()
        self._write(cmd)
        self._ser.timeout = timeout
        line = self._ser.readline()
        if not line.endswith(b"\n"):
            return None
        return line.rstrip(b"\r\n")

    def _query(self, cmd: str, timeout: float = QUERY_TIMEOUT_S, encoding: str = "utf-8") -> str | None:
        raw = self._query_raw(cmd, timeout)
        return None if raw is None else raw.decode(encoding, errors="replace").strip()

    def _drain(self, quiet_s: float = 0.1) -> None:
        """Discard input until the line has been quiet for quiet_s."""
        assert self._ser
        self._ser.timeout = quiet_s
        while self._ser.read(256):
            pass

    def _wait_ready(self, max_wait: float) -> float:
        """The meter ignores most commands while busy. Probe with a cheap query until it answers.

        While busy it sometimes answers a probe late instead of dropping it. A late reply would
        be mistaken for the answer to the next query (an AUTO? "1" read as MEAS? is a phantom
        1 V reading), so drain any trailing replies before returning.
        """
        t0 = time.monotonic()
        while time.monotonic() - t0 < max_wait:
            if self._query_raw("AUTO?", READY_PROBE_TIMEOUT_S) is not None:
                self._drain()
                return time.monotonic() - t0
        raise MeterError("meter_timeout", f"meter still busy after {max_wait:.1f} s")

    def _send(self, cmd: str, max_wait: float = 2.0) -> None:
        self._write(cmd)
        time.sleep(0.015)
        self._wait_ready(max_wait)

    # ------------------------------------------------------------------ state
    def _refresh_state(self, full: bool = False) -> set[str]:
        """Query configuration. Returns the set of fields that changed."""
        new: dict[str, Any] = {}
        func = self._query("FUNC?")
        if func is not None and func.startswith('"') and func.strip('"') in FUNC_BY_REPLY:
            new["function"] = FUNC_BY_REPLY[func.strip('"')]
        rng = self._query("RANGE?")
        if valid_range_label(rng):
            new["range_label"] = rng
            new["range"] = parse_range(rng)
        auto = self._query("AUTO?")
        if auto in ("0", "1"):
            new["autorange"] = auto == "1"
        rate = self._query("RATE?")
        if rate in RATE_BY_CODE:
            new["rate"] = RATE_BY_CODE[rate]
        if full:
            beep = self._query("SYST:BEEP?", timeout=1.0)
            if beep in ("ON", "OFF"):
                new["beep"] = beep == "ON"
            unit = self._query("TEMP:RTD:UNIT?")
            if unit:
                new["temp_unit"] = unit
        changed = {k for k, v in new.items() if self.state.get(k) != v}
        self.state.update(new)
        if changed & {"function", "range_label"} and not full:
            self._begin_settle()
        elif changed:
            self._emit_state()
        return changed

    def _begin_settle(self) -> None:
        self._settle_start = time.monotonic()
        self.state["settled"] = False
        self._emit_state()

    def _interval(self) -> float:
        return UPDATE_INTERVAL_S.get(self.state.get("rate") or "slow", 1.0)

    # ------------------------------------------------------------------ polling
    def _poll(self) -> None:
        now = time.monotonic()
        if not self.responding:
            if now >= self._next_state_poll:
                self._next_state_poll = now + 2.0
                self._on_responding()
            return

        meas = self._query("MEAS?")
        if meas is None:
            self._misses += 1
            if self._misses >= MISSES_BEFORE_OFFLINE:
                self.responding = False
                self.state["settled"] = False
                self._emit_status()
            return
        self._misses = 0
        if not valid_measurement(meas):
            self._drain()  # out of step with the meter; resynchronise
            return

        rng = self._query("RANGE?")
        display = self._query("MEAS:SHOW?", encoding="gb2312")  # Ω arrives as GB2312 here
        if valid_range_label(rng) and rng != self.state["range_label"]:
            # Front-panel change or an autorange step: readings are stale for a while.
            self.state["range_label"] = rng
            self.state["range"] = parse_range(rng)
            self._begin_settle()
        if now >= self._next_state_poll:
            self._next_state_poll = now + STATE_POLL_PERIOD_S
            self._refresh_state()

        changed = meas != self._last_raw
        self._last_raw = meas

        if self._settle_start is not None:
            elapsed = now - self._settle_start
            if (changed and elapsed >= SETTLE_MIN_S) or elapsed >= SETTLE_MAX_S:
                self._settle_start = None
                self.state["settled"] = True
                self._emit_state()
                self._emit_sample(meas, display)
            return

        # A stable input repeats the same value, so also emit once per update interval.
        if changed or (now - self._last_emit) >= self._interval() * 1.5:
            self._emit_sample(meas, display)

    def _emit_sample(self, raw: str, display: str | None) -> None:
        try:
            value = float(raw)
        except ValueError:
            return
        func = self.state["function"]
        overload = math.isinf(value) or abs(value) >= OVERLOAD_THRESHOLD
        unit = FUNCTIONS[func][2] if func in FUNCTIONS else None
        if func == "temp" and self.state.get("temp_unit"):
            unit = self.state["temp_unit"]
        self.seq += 1
        self._last_emit = time.monotonic()
        sample = {
            "value": None if overload else value,
            "unit": unit,
            "function": func,
            "range_label": self.state["range_label"],
            "overload": overload,
            "display": display,
            "seq": self.seq,
            "t_ms": self.millis(),
            "ts": time.time(),
        }
        self.last_sample = sample
        self.on_event({"type": "reading", **sample})

    def _emit_state(self) -> None:
        self.on_event({"type": "state", **self.snapshot()})

    def _emit_status(self) -> None:
        self.on_event({"type": "status", "connected": self.connected, "port_open": self.port_open})

    # ------------------------------------------------------------------ jobs
    def _verify(self, what: str, check: Callable[[], bool], resend: Callable[[], None]) -> None:
        if check():
            return
        resend()
        if not check():
            raise MeterError("verify_failed", f"meter did not accept {what}", state=self.snapshot())

    def _do_configure(self, function, range_, rate, beep) -> dict[str, Any]:
        self._refresh_state()
        changed: list[str] = []

        if function is not None and function != self.state["function"]:
            cmd = FUNCTIONS[function][0]
            self._send(cmd)
            self._verify(f"function {function}",
                         lambda: FUNC_BY_REPLY.get((self._query("FUNC?") or "").strip('"')) == function,
                         lambda: self._send(cmd))
            self._refresh_state()
            self._begin_settle()
            changed.append("function")

        target_func = function or self.state["function"]
        if range_ is not None:
            if range_ == "auto":
                if not self.state["autorange"]:
                    self._send("AUTO", max_wait=1.0)
                    self._verify("autorange", lambda: self._query("AUTO?") == "1",
                                 lambda: self._send("AUTO", max_wait=1.0))
                    self.state["autorange"] = True
                    changed.append("range")
            else:
                # RANGE <n> is fast (about 55 ms) and doesn't reconfigure the function.
                index, full_scale = select_range(target_func, range_)
                cur = self.state["range"]
                same = (not self.state["autorange"] and cur is not None
                        and math.isclose(cur, full_scale, rel_tol=1e-6))
                if not same:
                    cmd = f"RANGE {index}"
                    self._send(cmd, max_wait=1.0)

                    def range_ok() -> bool:
                        self._refresh_state()
                        r = self.state["range"]
                        return (self.state["autorange"] is False and r is not None
                                and math.isclose(r, full_scale, rel_tol=1e-6))

                    self._verify(f"range {full_scale:g}", range_ok, lambda: self._send(cmd, max_wait=1.0))
                    self._begin_settle()
                    changed.append("range")

        if rate is not None and rate != self.state["rate"]:
            cmd = f"RATE {RATES[rate]}"
            self._send(cmd)
            self._verify(f"rate {rate}", lambda: self._query("RATE?") == RATES[rate],
                         lambda: self._send(cmd))
            self.state["rate"] = rate
            changed.append("rate")

        if beep is not None and beep != self.state["beep"]:
            want = "ON" if beep else "OFF"
            cmd = f"SYST:BEEP {want}"
            self._send(cmd)
            self._verify(f"beep {want}", lambda: self._query("SYST:BEEP?", timeout=1.0) == want,
                         lambda: self._send(cmd))
            self.state["beep"] = beep
            changed.append("beep")

        if changed:
            self._emit_state()
        return {"changed": changed, "seq": self.seq, "settling": self._settle_start is not None}

    def _do_scpi(self, cmd: str) -> dict[str, Any]:
        if "?" in cmd:
            reply = self._query(cmd, timeout=1.0)
        else:
            self._send(cmd)
            reply = None
        self._refresh_state(full=True if cmd.upper().startswith("SYST") else False)
        return {"reply": reply, "seq": self.seq}
