"""Minimal MCP server (Streamable HTTP transport, JSON responses only, stateless).

Deliberately hand-rolled with no MCP SDK, so the exact same logic can be ported to the
ESP32: one POST endpoint, JSON-RPC 2.0 in, JSON out. No SSE stream is offered, which the
spec allows (GET returns 405).
"""

from __future__ import annotations

import json
from typing import Any

from .api import BRIDGE_VERSION, Bridge
from .meter import FUNCTIONS, RATES, MeterError

SUPPORTED_VERSIONS = ["2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"]

INSTRUCTIONS = (
    "Digcreat XDM1041 bench multimeter (5.5 digit) on the local network. Use `measure` for "
    "almost everything: it configures the meter, waits until readings are valid, and returns "
    "stats. Values are SI units (V, A, ohm, F, Hz, s). New readings arrive about 1/s at rate "
    "slow, 1.5/s medium, 3/s fast; a function or range change takes about 3 s to settle. "
    "The bridge cannot see where the test leads are plugged in. The user tells it which jack the "
    "red lead is in (get_state: state.leads is unknown, none, input, ma or 10a), and the bridge "
    "refuses functions and ranges that do not suit that jack with the error leads_mismatch. When "
    "that happens, ask the user to move the lead and update the setting on the dashboard; do not "
    "try to work around it. Current (dci/aci) also needs the meter in series with the load: never "
    "select a current function to measure across a voltage source."
)

_FUNC_DESC = ("Measurement function: dcv/acv (volts), dci/aci (amps), res (ohms), cap (farads), "
              "freq (Hz), period (s), diode (V), continuity (ohms), temp (degrees)")
_RANGE_SCHEMA = {
    "description": ('"auto" (default when a function is given), or full-scale range in SI units, '
                    "e.g. 0.5 for the 500 mV range or 5000 for 5 kohm. Rounds up to the next "
                    "available range. Only for dcv, acv, dci, aci, res, cap."),
    "anyOf": [{"type": "string", "enum": ["auto"]}, {"type": "number", "exclusiveMinimum": 0}],
}

TOOLS = [
    {
        "name": "measure",
        "title": "Measure",
        "description": ("Take a measurement. Configures function/range/rate if needed, waits for "
                        "valid readings, collects `samples` fresh readings and returns them with "
                        "mean/min/max/stdev. `overload` true means out of range or open input."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "function": {"type": "string", "enum": list(FUNCTIONS), "description": _FUNC_DESC},
                "range": _RANGE_SCHEMA,
                "rate": {"type": "string", "enum": list(RATES),
                         "description": "Reading rate; slow is the most accurate"},
                "samples": {"type": "integer", "minimum": 1, "maximum": 100, "default": 1},
            },
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": False, "idempotentHint": True},
    },
    {
        "name": "configure",
        "title": "Configure meter",
        "description": ("Change meter settings without taking a measurement. Returns the "
                        "verified state and the first valid reading."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "function": {"type": "string", "enum": list(FUNCTIONS), "description": _FUNC_DESC},
                "range": _RANGE_SCHEMA,
                "rate": {"type": "string", "enum": list(RATES)},
                "beep": {"type": "boolean", "description": "Front-panel beeper on/off"},
            },
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": False, "idempotentHint": True},
    },
    {
        "name": "get_reading",
        "title": "Latest reading",
        "description": ("Return the latest valid reading without changing anything. "
                        "Set fresh=true to wait for a reading taken after this call."),
        "inputSchema": {
            "type": "object",
            "properties": {"fresh": {"type": "boolean", "default": False}},
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": True},
    },
    {
        "name": "get_state",
        "title": "Meter state",
        "description": ("Return meter identity and current settings (function, range, autorange, rate) "
                        "and where the user says the red lead is (leads)."),
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
        "annotations": {"readOnlyHint": True},
    },
    {
        "name": "scpi",
        "title": "Raw SCPI command",
        "description": ("Send a raw SCPI command for features not covered by other tools. Queries "
                        "(containing '?') return the reply, or null if the meter does not support "
                        "the command. Prefer `measure` and `configure`."),
        "inputSchema": {
            "type": "object",
            "properties": {"command": {"type": "string", "description": "e.g. CONT:THRE?"}},
            "required": ["command"],
            "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": False, "destructiveHint": False},
    },
]
_TOOL_NAMES = {t["name"] for t in TOOLS}


def _ok(rid: Any, result: dict) -> dict:
    return {"jsonrpc": "2.0", "id": rid, "result": result}


def _err(rid: Any, code: int, message: str) -> dict:
    return {"jsonrpc": "2.0", "id": rid, "error": {"code": code, "message": message}}


async def _call_tool(bridge: Bridge, name: str, args: dict[str, Any]) -> dict:
    if name in ("measure", "configure") and "force" in args:
        raise MeterError("bad_request", "force is not available to agents: ask the user to move the "
                         "lead and update the lead setting on the dashboard")
    if name == "measure":
        return await bridge.measure(args)
    if name == "configure":
        return await bridge.config(args)
    if name == "get_reading":
        unknown = set(args) - {"fresh"}
        if unknown:
            raise MeterError("bad_request", f"unknown field(s): {', '.join(sorted(unknown))}")
        return await bridge.reading(fresh=bool(args.get("fresh", False)))
    if name == "get_state":
        return {"info": bridge.info(), "state": bridge.state()}
    if name == "scpi":
        return await bridge.scpi(args.get("command"))
    raise KeyError(name)


async def handle_message(bridge: Bridge, msg: Any) -> dict | None:
    """Handle one JSON-RPC message. Returns the response, or None for notifications."""
    if not isinstance(msg, dict) or msg.get("jsonrpc") != "2.0" or "method" not in msg:
        if isinstance(msg, dict) and ("result" in msg or "error" in msg):
            return None  # a response to a server request; we never send any
        return _err(msg.get("id") if isinstance(msg, dict) else None, -32600, "Invalid Request")
    method, rid, params = msg["method"], msg.get("id"), msg.get("params") or {}
    if "id" not in msg:
        return None  # notification (notifications/initialized, cancelled, ...)

    if method == "initialize":
        requested = params.get("protocolVersion")
        version = requested if requested in SUPPORTED_VERSIONS else SUPPORTED_VERSIONS[0]
        return _ok(rid, {
            "protocolVersion": version,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": "metermind", "title": "MeterMind (XDM1041 multimeter)",
                           "version": BRIDGE_VERSION},
            "instructions": INSTRUCTIONS,
        })
    if method == "ping":
        return _ok(rid, {})
    if method == "tools/list":
        return _ok(rid, {"tools": TOOLS})
    if method == "tools/call":
        name = params.get("name")
        args = params.get("arguments") or {}
        if name not in _TOOL_NAMES:
            return _err(rid, -32602, f"Unknown tool: {name}")
        if not isinstance(args, dict):
            return _err(rid, -32602, "arguments must be an object")
        try:
            result = await _call_tool(bridge, name, args)
        except MeterError as e:
            err = {"error": e.code, "message": e.message, **e.extra}
            return _ok(rid, {"content": [{"type": "text", "text": json.dumps(err)}],
                             "isError": True})
        return _ok(rid, {"content": [{"type": "text", "text": json.dumps(result)}],
                         "structuredContent": result, "isError": False})
    return _err(rid, -32601, f"Method not found: {method}")
