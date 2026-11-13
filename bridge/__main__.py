"""Run the bridge: python -m bridge [--serial PORT] [--http-port 8041] [--scpi-port 5025] [--mdns]"""

import argparse
import asyncio
import logging

from aiohttp import web

from .api import Bridge
from .meter import Meter
from .server import make_app, start_mdns, start_scpi_server

log = logging.getLogger("bridge")


async def main() -> None:
    ap = argparse.ArgumentParser(prog="python -m bridge", description=__doc__)
    ap.add_argument("--serial", help="serial port (default: auto-detect the CH340)")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--http-port", type=int, default=8041)
    ap.add_argument("--scpi-port", type=int, default=5025, help="0 disables raw SCPI")
    ap.add_argument("--mdns", action="store_true", help="advertise as <name>.local")
    ap.add_argument("--name", default="metermind")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")

    meter = Meter(port=a.serial)
    bridge = Bridge(meter)
    bridge.attach(asyncio.get_running_loop())
    meter.start()

    runner = web.AppRunner(make_app(bridge), access_log=None)
    await runner.setup()
    await web.TCPSite(runner, a.host, a.http_port).start()
    log.info("HTTP/WebSocket/MCP on http://%s:%d  (MCP: /mcp)", a.host, a.http_port)
    scpi = None
    if a.scpi_port:
        scpi = await start_scpi_server(bridge, a.host, a.scpi_port)
        log.info("raw SCPI on tcp port %d", a.scpi_port)
    zc = None
    if a.mdns:
        zc, _ = await start_mdns(a.name, a.http_port, a.scpi_port)
        log.info("mDNS: advertising %s.local", a.name)
    try:
        await asyncio.Event().wait()
    finally:
        if zc:
            await zc.async_unregister_all_services()
            await zc.async_close()
        if scpi:
            scpi.close()
        await runner.cleanup()
        meter.stop()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
