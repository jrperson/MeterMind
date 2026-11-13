"""Transports: HTTP REST, WebSocket, MCP (all on one port) and raw SCPI over TCP."""

from __future__ import annotations

import asyncio
import json
import logging
import socket
from pathlib import Path
from urllib.parse import urlsplit

from aiohttp import WSMsgType, web

from . import mcp
from .api import BRIDGE_VERSION, HTTP_STATUS, Bridge
from .meter import MeterError

log = logging.getLogger("bridge")
STATIC = Path(__file__).parent / "static"
BRIDGE_KEY = web.AppKey("bridge", Bridge)


def _json(data, status: int = 200) -> web.Response:
    return web.json_response(data, status=status, dumps=lambda o: json.dumps(o, ensure_ascii=False))


def _error(e: MeterError) -> web.Response:
    return _json({"error": e.code, "message": e.message, **e.extra}, HTTP_STATUS.get(e.code, 500))


@web.middleware
async def guard(request: web.Request, handler):
    # DNS-rebinding protection (required by the MCP spec, sensible for the whole API): a
    # browser page from another site may not drive the meter. Non-browser clients send no Origin.
    origin = request.headers.get("Origin")
    if origin:
        o = urlsplit(origin)
        if o.netloc != request.host and o.hostname not in ("localhost", "127.0.0.1"):
            return _json({"error": "forbidden", "message": "cross-origin request refused"}, 403)
    try:
        return await handler(request)
    except MeterError as e:
        return _error(e)


async def _body(request: web.Request) -> dict:
    if not request.can_read_body:
        return {}
    try:
        return await request.json()
    except (json.JSONDecodeError, UnicodeDecodeError):
        raise MeterError("bad_request", "body is not valid JSON")


# ---------------------------------------------------------------------- REST
async def index(request: web.Request) -> web.StreamResponse:
    return web.FileResponse(STATIC / "index.html")


async def api_index(request: web.Request) -> web.Response:
    return _json({
        "name": "metermind", "version": BRIDGE_VERSION,
        "endpoints": {
            "GET /api/info": "identity and connection status",
            "GET /api/state": "current settings",
            "GET /api/reading?fresh=1&timeout_ms=5000": "latest valid reading",
            "PUT /api/config": "{function, range, rate, beep}: set, verify, wait for settle",
            "POST /api/measure": "{function, range, rate, samples}: configure + sample + stats",
            "POST /api/scpi": "{cmd}: raw SCPI escape hatch",
            "GET /api/leads": "where the user says the red lead is",
            "PUT /api/leads": "{red: unknown|none|input|ma|10a}: set it (config and measure check it)",
            "GET /ws": "WebSocket: reading/state/status events, accepts {id, op, ...}",
            "POST /mcp": "MCP endpoint (Streamable HTTP, JSON responses)",
        },
    })


async def api_info(request: web.Request) -> web.Response:
    return _json(request.app[BRIDGE_KEY].info())


async def api_state(request: web.Request) -> web.Response:
    return _json(request.app[BRIDGE_KEY].state())


async def api_reading(request: web.Request) -> web.Response:
    fresh = request.query.get("fresh", "0").lower() in ("1", "true", "yes")
    try:
        timeout_ms = int(request.query.get("timeout_ms", "5000"))
    except ValueError:
        raise MeterError("bad_request", "timeout_ms must be an integer")
    return _json(await request.app[BRIDGE_KEY].reading(fresh=fresh, timeout_ms=timeout_ms))


async def api_config(request: web.Request) -> web.Response:
    return _json(await request.app[BRIDGE_KEY].config(await _body(request)))


async def api_measure(request: web.Request) -> web.Response:
    return _json(await request.app[BRIDGE_KEY].measure(await _body(request)))


async def api_leads(request: web.Request) -> web.Response:
    bridge = request.app[BRIDGE_KEY]
    if request.method == "GET":
        return _json(bridge.leads())
    return _json(bridge.set_leads(await _body(request)))


async def api_scpi(request: web.Request) -> web.Response:
    body = await _body(request)
    return _json(await request.app[BRIDGE_KEY].scpi(body.get("cmd")))


# ---------------------------------------------------------------------- WebSocket
async def websocket(request: web.Request) -> web.WebSocketResponse:
    bridge: Bridge = request.app[BRIDGE_KEY]
    ws = web.WebSocketResponse(heartbeat=20)
    await ws.prepare(request)
    q = bridge.subscribe()
    tasks: set[asyncio.Task] = set()

    async def send(obj) -> None:
        if not ws.closed:
            await ws.send_str(json.dumps(obj, ensure_ascii=False))

    async def pump() -> None:
        while True:
            await send(await q.get())

    async def run_op(msg: dict) -> None:
        rid = msg.get("id")
        op = msg.get("op")
        args = {k: v for k, v in msg.items() if k not in ("id", "op")}
        try:
            if op == "config":
                res = await bridge.config(args)
            elif op == "measure":
                res = await bridge.measure(args)
            elif op == "reading":
                res = await bridge.reading(fresh=bool(args.get("fresh")),
                                           timeout_ms=int(args.get("timeout_ms", 5000)))
            elif op == "state":
                res = bridge.state()
            elif op == "info":
                res = bridge.info()
            elif op == "scpi":
                res = await bridge.scpi(args.get("cmd"))
            elif op == "leads":
                res = bridge.set_leads(args) if args else bridge.leads()
            else:
                raise MeterError("bad_request", f"unknown op: {op}")
            await send({"id": rid, "ok": True, **res})
        except MeterError as e:
            await send({"id": rid, "ok": False, "error": e.code, "message": e.message, **e.extra})

    hello = {"type": "hello", "info": bridge.info(), "leads": bridge.leads()}
    if bridge.meter.connected:
        hello["state"] = bridge.state()
        if bridge.meter.last_sample:
            hello["reading"] = bridge.meter.last_sample
    await send(hello)
    pump_task = asyncio.create_task(pump())
    try:
        async for m in ws:
            if m.type != WSMsgType.TEXT:
                continue
            try:
                msg = json.loads(m.data)
                assert isinstance(msg, dict)
            except (json.JSONDecodeError, AssertionError):
                await send({"ok": False, "error": "bad_request", "message": "messages must be JSON objects"})
                continue
            t = asyncio.create_task(run_op(msg))
            tasks.add(t)
            t.add_done_callback(tasks.discard)
    finally:
        pump_task.cancel()
        for t in tasks:
            t.cancel()
        bridge.unsubscribe(q)
    return ws


# ---------------------------------------------------------------------- MCP
async def mcp_post(request: web.Request) -> web.Response:
    bridge: Bridge = request.app[BRIDGE_KEY]
    try:
        payload = await request.json()
    except (json.JSONDecodeError, UnicodeDecodeError):
        return _json({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Parse error"}}, 400)
    if isinstance(payload, list):  # batches (older protocol versions)
        replies = [r for r in [await mcp.handle_message(bridge, m) for m in payload] if r]
        return _json(replies) if replies else web.Response(status=202)
    reply = await mcp.handle_message(bridge, payload)
    return _json(reply) if reply else web.Response(status=202)


async def mcp_not_allowed(request: web.Request) -> web.Response:
    return web.Response(status=405, headers={"Allow": "POST"})


def make_app(bridge: Bridge) -> web.Application:
    app = web.Application(middlewares=[guard])
    app[BRIDGE_KEY] = bridge
    app.add_routes([
        web.get("/", index),
        web.get("/api", api_index),
        web.get("/api/info", api_info),
        web.get("/api/state", api_state),
        web.get("/api/reading", api_reading),
        web.put("/api/config", api_config),
        web.post("/api/config", api_config),
        web.post("/api/measure", api_measure),
        web.post("/api/scpi", api_scpi),
        web.get("/api/leads", api_leads),
        web.put("/api/leads", api_leads),
        web.get("/ws", websocket),
        web.post("/mcp", mcp_post),
        web.get("/mcp", mcp_not_allowed),
        web.delete("/mcp", mcp_not_allowed),
    ])
    return app


# ---------------------------------------------------------------------- raw SCPI socket
async def start_scpi_server(bridge: Bridge, host: str, port: int) -> asyncio.base_events.Server:
    """Line-based SCPI on TCP (PyVISA: TCPIP::<host>::5025::SOCKET). Mirrors the meter:
    queries get one reply line, unsupported queries get no reply."""

    async def client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            while line := await reader.readline():
                cmd = line.decode("ascii", errors="replace").strip()
                if not cmd:
                    continue
                try:
                    reply = (await bridge.scpi(cmd))["reply"]
                except MeterError as e:
                    log.warning("scpi %r: %s", cmd, e.message)
                    reply = None
                if reply is not None:
                    writer.write(reply.encode("utf-8") + b"\n")
                    await writer.drain()
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            writer.close()

    return await asyncio.start_server(client, host, port)


# ---------------------------------------------------------------------- mDNS
def _lan_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))  # no packet is sent; just picks the outbound interface
        return s.getsockname()[0]
    finally:
        s.close()


async def start_mdns(name: str, http_port: int, scpi_port: int):
    from zeroconf import ServiceInfo
    from zeroconf.asyncio import AsyncZeroconf

    ip = socket.inet_aton(_lan_ip())
    zc = AsyncZeroconf()
    props = {"path": "/api", "mcp": "/mcp", "ws": "/ws", "version": BRIDGE_VERSION}
    infos = [
        ServiceInfo("_http._tcp.local.", f"{name}._http._tcp.local.", addresses=[ip],
                    port=http_port, properties=props, server=f"{name}.local."),
        ServiceInfo("_metermind._tcp.local.", f"{name}._metermind._tcp.local.", addresses=[ip],
                    port=http_port, properties=props, server=f"{name}.local."),
    ]
    if scpi_port:
        infos.append(ServiceInfo("_scpi-raw._tcp.local.", f"{name}._scpi-raw._tcp.local.",
                                 addresses=[ip], port=scpi_port, server=f"{name}.local."))
    for info in infos:
        await zc.async_register_service(info)
    return zc, infos
