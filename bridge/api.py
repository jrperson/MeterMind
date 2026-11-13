"""Operation layer shared by every transport (HTTP, WebSocket, MCP, raw SCPI).

Each public coroutine here is one API operation. Transports only translate their wire format
to these calls, so behaviour is identical whichever way a client connects.
"""

from __future__ import annotations

import asyncio
import math
import statistics
import time
from typing import Any

from .meter import (FUNCTIONS, RANGES, RATES, SETTLE_MAX_S, UPDATE_INTERVAL_S, Meter, MeterError,
                    select_range)

BRIDGE_VERSION = "0.2.0"
MAX_SAMPLES = 100
WAIT_EXTEND_MAX_S = 12.0  # how far a settle can push a reading wait past its deadline
HTTP_STATUS = {
    "bad_request": 400, "busy": 409, "leads_mismatch": 409, "internal": 500, "verify_failed": 502,
    "meter_disconnected": 503, "meter_timeout": 504,
}

CONFIG_KEYS = {"function", "range", "rate", "beep"}
MEASURE_KEYS = CONFIG_KEYS | {"samples", "timeout_ms"}

# Where the user says the red lead is (the black one is always in COM). The bridge cannot see the
# leads; this is what the user told it, "unknown" until they do (and again after a restart).
LEADS = ("unknown", "none", "input", "ma", "10a")
JACK_NAME = {"input": "Input", "ma": "µA mA", "10a": "10A"}
FUNC_TITLE = {
    "dcv": "DC volts", "acv": "AC volts", "dci": "DC current", "aci": "AC current", "res": "Resistance",
    "cap": "Capacitance", "freq": "Frequency", "period": "Period", "diode": "Diode test",
    "continuity": "Continuity", "temp": "Temperature",
}


def _amps(fs: float) -> str:
    return f"{fs * 1e6:g} µA" if fs < 1e-3 else f"{fs * 1e3:g} mA" if fs < 1 else f"{fs:g} A"


def leads_problem(red: str, function: str | None, manual_fs: float | None) -> str | None:
    """Why `function` (on manual range `manual_fs`, None for auto) does not suit the red lead's jack."""
    if red == "none" or function is None:
        return None
    current = function in ("dci", "aci")
    title = FUNC_TITLE.get(function, function)
    fix = " Move the lead and update the lead setting (dashboard or PUT /api/leads) first."
    if red == "unknown" and current:
        return (f"{title} needs the red lead in the µA mA or 10A jack, and no lead position is set."
                " Set where the leads are (dashboard or PUT /api/leads) first.")
    if red == "input" and current:
        return f"{title} needs the red lead in the µA mA or 10A jack, but it is set to the Input jack." + fix
    if red in ("ma", "10a") and not current:
        return (f"{title} needs the red lead in the Input jack, but it is set to the {JACK_NAME[red]} jack:"
                " measuring there puts the input across the current shunt and can blow the fuse." + fix)
    if red == "ma" and current and manual_fs is not None and manual_fs >= 5:
        return f"The {_amps(manual_fs)} range needs the red lead in the 10A jack, but it is set to the µA mA jack." + fix
    if red == "10a" and current and manual_fs is not None and manual_fs <= 0.5:
        return f"The {_amps(manual_fs)} range needs the red lead in the µA mA jack, but it is set to the 10A jack." + fix
    return None


def _take_force(body: Any) -> tuple[Any, bool]:
    """Split the optional "force" flag (skip the lead check) off a config or measure body."""
    if not isinstance(body, dict) or "force" not in body:
        return body, False
    body = dict(body)
    force = body.pop("force")
    if not isinstance(force, bool):
        raise MeterError("bad_request", "force must be true or false")
    return body, force


def normalize_config(body: dict[str, Any], current_function: str | None,
                     allowed: set[str] = CONFIG_KEYS) -> dict[str, Any]:
    """Validate a config request. Raises MeterError('bad_request').

    If a function is given without a range, the range defaults to "auto": each meter function
    silently remembers its last manual range, which is a trap for automated clients.
    """
    if not isinstance(body, dict):
        raise MeterError("bad_request", "body must be a JSON object")
    unknown = set(body) - allowed
    if unknown:
        raise MeterError("bad_request", f"unknown field(s): {', '.join(sorted(unknown))}",
                         allowed=sorted(allowed))
    out: dict[str, Any] = {"function": None, "range_": None, "rate": None, "beep": None}

    func = body.get("function")
    if func is not None:
        if func not in FUNCTIONS:
            raise MeterError("bad_request", f"function must be one of {', '.join(FUNCTIONS)}")
        out["function"] = func
    target = func or current_function

    rng = body.get("range")
    if rng is None and func is not None and FUNCTIONS[func][3]:
        rng = "auto"
    if rng is not None:
        if target in FUNCTIONS and not FUNCTIONS[target][3]:
            if body.get("range") is not None:
                raise MeterError("bad_request", f"function {target} has no selectable range")
            rng = None
        elif rng == "auto":
            pass
        elif isinstance(rng, (int, float)) and not isinstance(rng, bool) and math.isfinite(rng) and rng > 0:
            rng = float(rng)
            if target in RANGES:
                select_range(target, rng)  # raises bad_request if above the top range
        else:
            raise MeterError("bad_request", 'range must be "auto" or a positive number in SI units')
        out["range_"] = rng

    rate = body.get("rate")
    if rate is not None:
        if rate not in RATES:
            raise MeterError("bad_request", f"rate must be one of {', '.join(RATES)}")
        out["rate"] = rate

    beep = body.get("beep")
    if beep is not None:
        if not isinstance(beep, bool):
            raise MeterError("bad_request", "beep must be true or false")
        out["beep"] = beep
    return out


class Bridge:
    def __init__(self, meter: Meter):
        self.meter = meter
        self.loop: asyncio.AbstractEventLoop | None = None
        self._subs: set[asyncio.Queue] = set()
        self._op_lock = asyncio.Lock()  # config/measure/scpi run one at a time
        self._started = time.monotonic()
        self.red_lead = "unknown"

    def attach(self, loop: asyncio.AbstractEventLoop) -> None:
        self.loop = loop
        self.meter.on_event = lambda e: loop.call_soon_threadsafe(self._dispatch, e)

    # ------------------------------------------------------------------ events
    def subscribe(self, maxsize: int = 256) -> asyncio.Queue:
        q: asyncio.Queue = asyncio.Queue(maxsize=maxsize)
        self._subs.add(q)
        return q

    def unsubscribe(self, q: asyncio.Queue) -> None:
        self._subs.discard(q)

    def _dispatch(self, event: dict) -> None:
        for q in list(self._subs):
            if q.full():  # slow consumer: drop its oldest event rather than block the meter
                try:
                    q.get_nowait()
                except asyncio.QueueEmpty:
                    pass
            q.put_nowait(event)

    async def _next_readings(self, q: asyncio.Queue, after_seq: int, n: int, timeout: float,
                             extend: tuple[float, float] | None = None) -> list[dict]:
        """Collects n readings with seq > after_seq from q.

        With extend = (base, per_reading), each settle that starts while waiting moves the
        deadline to base + per_reading for each reading still to come, counted from then, but
        never more than WAIT_EXTEND_MAX_S past the original deadline. Every autorange step starts
        a new settle (about 1.8 s each), so a fixed deadline fails a long climb, e.g. res from
        500 ohm to 50 Mohm on an open input, although the meter gets there.
        """
        out: list[dict] = []
        deadline = time.monotonic() + timeout
        limit = deadline + WAIT_EXTEND_MAX_S
        while len(out) < n:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise MeterError("meter_timeout", f"got {len(out)} of {n} readings before timeout",
                                 readings=out)
            try:
                ev = await asyncio.wait_for(q.get(), remaining)
            except asyncio.TimeoutError:
                continue
            if ev["type"] == "status" and not ev["connected"]:
                raise MeterError("meter_disconnected", "meter disconnected while waiting")
            if extend and ev["type"] == "state" and not ev["settled"]:
                base, per_reading = extend
                deadline = max(deadline, min(time.monotonic() + base + (n - len(out)) * per_reading, limit))
            if ev["type"] == "reading" and ev["seq"] > after_seq:
                out.append(_reading(ev))
        return out

    # ------------------------------------------------------------------ operations
    def _require_connected(self) -> None:
        if not self.meter.connected:
            raise MeterError("meter_disconnected", "meter is not connected")

    def info(self) -> dict[str, Any]:
        m = self.meter
        return {
            "model": m.info.get("model"), "manufacturer": m.info.get("manufacturer"),
            "serial": m.info.get("serial"), "meter_fw": m.info.get("meter_fw"),
            "bridge_fw": BRIDGE_VERSION, "connected": m.connected,
            "uptime_s": int(time.monotonic() - self._started),
        }

    def state(self) -> dict[str, Any]:
        self._require_connected()
        s = self.meter.snapshot()
        s.pop("temp_unit", None)
        s["leads"] = self.red_lead
        return s

    def leads(self) -> dict[str, Any]:
        return {"red": self.red_lead}

    def set_leads(self, body: Any) -> dict[str, Any]:
        if not isinstance(body, dict):
            raise MeterError("bad_request", "body must be a JSON object")
        unknown = set(body) - {"red"}
        if unknown:
            raise MeterError("bad_request", f"unknown field(s): {', '.join(sorted(unknown))}", allowed=["red"])
        red = body.get("red")
        if red not in LEADS:
            raise MeterError("bad_request", f"red must be one of {', '.join(LEADS)}")
        self.red_lead = red
        self._dispatch({"type": "leads", "red": red})
        return self.leads()

    def _check_leads(self, args: dict[str, Any]) -> None:
        """Refuse a function or range change that does not suit where the user says the lead is."""
        if args["function"] is None and args["range_"] is None:
            return
        func = args["function"] or self.meter.state["function"]
        rng = args["range_"]
        if rng == "auto":
            fs = None
        elif rng is not None:
            fs = select_range(func, rng)[1] if func in RANGES else None
        else:
            fs = None if self.meter.state.get("autorange") else self.meter.state.get("range")
        problem = leads_problem(self.red_lead, func, fs)
        if problem:
            raise MeterError("leads_mismatch", problem, leads=self.red_lead, function=func)

    async def reading(self, fresh: bool = False, timeout_ms: int = 5000) -> dict[str, Any]:
        self._require_connected()
        if fresh:
            q = self.subscribe()
            try:
                return (await self._next_readings(q, self.meter.seq, 1, timeout_ms / 1000))[0]
            finally:
                self.unsubscribe(q)
        if not self.meter.state["settled"] or self.meter.last_sample is None:
            raise MeterError("busy", "meter is settling after a configuration change",
                             retry_after_ms=int(SETTLE_MAX_S * 1000))
        return _reading(self.meter.last_sample)

    async def config(self, body: dict[str, Any]) -> dict[str, Any]:
        self._require_connected()
        body, force = _take_force(body)
        args = normalize_config(body, self.meter.state["function"])
        if not force:
            self._check_leads(args)
        async with self._op_lock:
            return await self._config_locked(args)

    async def _config_locked(self, args: dict[str, Any]) -> dict[str, Any]:
        q = self.subscribe()
        try:
            res = await asyncio.wrap_future(self.meter.configure(**args))
            if res["settling"] or "function" in res["changed"] or "range" in res["changed"]:
                reading = (await self._next_readings(q, res["seq"], 1, SETTLE_MAX_S + 4,
                                                     extend=(SETTLE_MAX_S + 4, 0)))[0]
            elif self.meter.last_sample is not None:
                reading = _reading(self.meter.last_sample)
            else:
                reading = (await self._next_readings(q, res["seq"], 1, 3))[0]
            return {"changed": res["changed"], "state": self.state(), "reading": reading}
        finally:
            self.unsubscribe(q)

    async def measure(self, body: dict[str, Any]) -> dict[str, Any]:
        self._require_connected()
        body, force = _take_force(body)
        if not isinstance(body, dict):
            raise MeterError("bad_request", "body must be a JSON object")
        samples = body.get("samples", 1)
        if not isinstance(samples, int) or isinstance(samples, bool) or not 1 <= samples <= MAX_SAMPLES:
            raise MeterError("bad_request", f"samples must be an integer from 1 to {MAX_SAMPLES}")
        unknown = set(body) - MEASURE_KEYS
        if unknown:
            raise MeterError("bad_request", f"unknown field(s): {', '.join(sorted(unknown))}",
                             allowed=sorted(MEASURE_KEYS))
        timeout_ms = body.get("timeout_ms")
        if timeout_ms is not None and (not isinstance(timeout_ms, (int, float)) or timeout_ms <= 0):
            raise MeterError("bad_request", "timeout_ms must be a positive number")
        args = normalize_config({k: v for k, v in body.items() if k in CONFIG_KEYS},
                                self.meter.state["function"])
        if not force:
            self._check_leads(args)
        t0 = time.monotonic()
        async with self._op_lock:
            q = self.subscribe()
            try:
                marker = self.meter.seq
                res = await asyncio.wrap_future(self.meter.configure(**args))
                if res["changed"]:
                    marker = res["seq"]
                rate = self.meter.state["rate"] or "slow"
                # An explicit timeout_ms is kept exactly; the default one follows the meter's settles.
                wait = (SETTLE_MAX_S + 3, UPDATE_INTERVAL_S[rate] * 2)
                timeout = timeout_ms / 1000 if timeout_ms else wait[0] + samples * wait[1]
                readings = await self._next_readings(q, marker, samples, timeout,
                                                     extend=None if timeout_ms else wait)
            finally:
                self.unsubscribe(q)
        values = [r["value"] for r in readings if not r["overload"]]
        out: dict[str, Any] = {
            "function": readings[0]["function"], "unit": readings[0]["unit"],
            "range_label": readings[-1]["range_label"],
            "samples": [r["value"] for r in readings],
            "overload": len(values) < len(readings),
            "mean": None, "min": None, "max": None, "stdev": None,
            "duration_ms": int((time.monotonic() - t0) * 1000),
        }
        if values:
            out.update(mean=statistics.fmean(values), min=min(values), max=max(values),
                       stdev=statistics.stdev(values) if len(values) > 1 else 0.0)
        return out

    async def scpi(self, cmd: Any) -> dict[str, Any]:
        self._require_connected()
        if not isinstance(cmd, str) or not cmd.strip() or "\n" in cmd or len(cmd) > 200:
            raise MeterError("bad_request", "cmd must be a single-line SCPI command")
        async with self._op_lock:
            res = await asyncio.wrap_future(self.meter.scpi(cmd.strip()))
        return {"reply": res["reply"]}


def _reading(sample: dict[str, Any]) -> dict[str, Any]:
    r = {k: v for k, v in sample.items() if k != "type"}
    r["age_ms"] = max(0, int((time.time() - r["ts"]) * 1000))
    return r
