"""End-to-end test of a running bridge against the real meter.

PROBES MUST BE DISCONNECTED. With open inputs, resistance reads overload and DC volts reads
about 0 V, which lets this test detect stale readings leaking through after a mode change. The
lead-setting checks also switch to DC current, which is harmless with nothing connected. The test
sets the lead setting to "none" while it runs and restores it afterwards.

    python tools/smoke_test.py [http://localhost:8041]
"""

from __future__ import annotations

import asyncio
import json
import sys
import time

import aiohttp

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://localhost:8041"
results: list[tuple[bool, str]] = []


def check(ok: bool, name: str, detail: object = "") -> None:
    results.append((ok, name))
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail != "" else ""))


async def main() -> None:
    async with aiohttp.ClientSession() as s:
        async def call(method: str, path: str, body=None, **kw):
            t = time.monotonic()
            async with s.request(method, BASE + path, json=body, **kw) as r:
                txt = await r.text()
                try:
                    data = json.loads(txt) if txt else None
                except json.JSONDecodeError:
                    data = txt
                return r.status, data, time.monotonic() - t

        # Watch every WebSocket event while the tests run.
        events: list[dict] = []
        ws = await s.ws_connect(BASE.replace("http", "ws") + "/ws")
        hello = await ws.receive_json()
        check(hello.get("type") == "hello", "ws hello", hello.get("info", {}).get("model"))

        async def collect():
            async for m in ws:
                if m.type == aiohttp.WSMsgType.TEXT:
                    ev = json.loads(m.data)
                    ev["_t"] = time.monotonic()
                    events.append(ev)
        collector = asyncio.create_task(collect())

        # Nothing is plugged in during this test; put the lead setting back at the end.
        _, d, _ = await call("GET", "/api/leads")
        leads_before = d["red"]
        await call("PUT", "/api/leads", {"red": "none"})

        def readings_between(t0: float, t1: float) -> list[dict]:
            return [e for e in events if e.get("type") == "reading" and t0 <= e["_t"] <= t1]

        # ---- function change to resistance: open input must read overload, never a number
        t0 = time.monotonic()
        st, d, dt = await call("PUT", "/api/config", {"function": "res"})
        check(st == 200 and d["state"]["function"] == "res" and d["state"]["autorange"],
              "config res", f"{dt:.2f}s changed={d.get('changed')}" if st == 200 else f"{dt:.2f}s HTTP {st} {d}")
        check(st == 200 and d["reading"]["overload"], "res first reading is overload (open input)",
              d.get("reading") if isinstance(d, dict) else d)
        await asyncio.sleep(2)
        # Only res-labelled readings count: a dcv reading taken just before the switch can still
        # arrive after t0 when the request is slow to reach the bridge.
        leaked = [e for e in readings_between(t0, time.monotonic())
                  if e["function"] == "res" and not e["overload"]]
        check(not leaked, "no stale readings leaked during res settle", leaked[:2])

        # ---- back to DC volts: open input reads ~0 V, never overload
        t0 = time.monotonic()
        st, d, dt = await call("PUT", "/api/config", {"function": "dcv"})
        check(st == 200 and d["state"]["function"] == "dcv", "config dcv",
              f"{dt:.2f}s" if st == 200 else f"{dt:.2f}s HTTP {st} {d}")
        check(st == 200 and not d["reading"]["overload"] and abs(d["reading"]["value"]) < 0.01,
              "dcv first reading is ~0 V", d["reading"]["value"] if st == 200 else d)
        await asyncio.sleep(2)
        bad = [e for e in readings_between(t0, time.monotonic())
               if e["function"] == "dcv" and (e["overload"] or abs(e["value"]) > 0.01)]
        check(not bad, "no stale readings leaked during dcv settle", bad[:2])

        # ---- no-op config must be fast
        st, d, dt = await call("PUT", "/api/config", {"function": "dcv"})
        check(st == 200 and d["changed"] == [] and dt < 1.0, "no-op config skips settle", f"{dt:.2f}s")

        # ---- manual range then back to auto
        st, d, dt = await call("PUT", "/api/config", {"range": 0.5})
        check(st == 200 and d["state"]["range"] == 0.5 and d["state"]["autorange"] is False,
              "manual range 0.5 -> 500 mV", f"{d['state']['range_label']} {dt:.2f}s")
        st, d, dt = await call("PUT", "/api/config", {"range": 30})
        check(st == 200 and d["state"]["range"] == 50, "range 30 rounds up to 50 V", d["state"]["range_label"])
        st, d, dt = await call("PUT", "/api/config", {"range": 2000})
        check(st == 400 and d["error"] == "bad_request", "range above max rejected", d.get("message"))
        st, d, dt = await call("PUT", "/api/config", {"range": "auto"})
        check(st == 200 and d["state"]["autorange"] is True, "autorange on", f"{dt:.2f}s")
        st, d, dt = await call("PUT", "/api/config", {"function": "cap", "range": 1e-6})
        check(st == 200 and d["state"]["range_label"] == "5uF", "cap range 1 uF -> 5uF",
              f"{d['state']['range_label']} {dt:.2f}s")

        # ---- function without range defaults to auto even if a manual range was remembered
        await call("PUT", "/api/config", {"function": "res", "range": 50000})
        await call("PUT", "/api/config", {"function": "dcv"})
        st, d, _ = await call("PUT", "/api/config", {"function": "res"})
        check(st == 200 and d["state"]["autorange"] is True, "function w/o range resets to autorange",
              d["state"]["range_label"])

        # ---- measure with fast rate
        st, d, dt = await call("POST", "/api/measure", {"function": "dcv", "rate": "fast", "samples": 6})
        check(st == 200 and len(d["samples"]) == 6 and d["mean"] is not None,
              "measure dcv fast x6", f"{dt:.2f}s mean={d.get('mean')} stdev={d.get('stdev')}")
        st, d, dt = await call("POST", "/api/measure", {"rate": "slow", "samples": 2})
        check(st == 200 and len(d["samples"]) == 2, "measure slow x2 (keeps function)", f"{dt:.2f}s")
        st, d, _ = await call("GET", "/api/state")
        check(d["rate"] == "slow" and d["function"] == "dcv", "state after measure", d)

        # ---- validation errors
        for body, name in [({"function": "volts"}, "bad function"),
                           ({"function": "freq", "range": 5}, "range on freq"),
                           ({"funtion": "dcv"}, "typo field"),
                           ({"range": -1}, "negative range"),
                           ({"rate": "turbo"}, "bad rate")]:
            st, d, _ = await call("PUT", "/api/config", body)
            check(st == 400 and d["error"] == "bad_request", f"rejects {name}", d.get("message"))
        st, d, _ = await call("POST", "/api/measure", {"samples": 0})
        check(st == 400, "rejects samples=0")
        async with s.put(BASE + "/api/config", data="{not json") as r:
            check(r.status == 400, "rejects invalid JSON")

        # ---- scpi escape hatch
        st, d, _ = await call("POST", "/api/scpi", {"cmd": "CONT:THRE?"})
        check(st == 200 and d["reply"] == "50", "scpi CONT:THRE?", d)
        st, d, dt = await call("POST", "/api/scpi", {"cmd": "READ?"})
        check(st == 200 and d["reply"] is None, "scpi unsupported query -> null", f"{dt:.2f}s")

        # ---- origin guard
        st, d, _ = await call("GET", "/api/state", headers={"Origin": "http://evil.example"})
        check(st == 403, "cross-origin request refused")

        # ---- MCP
        hdr = {"Accept": "application/json, text/event-stream"}
        st, d, _ = await call("POST", "/mcp", {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                              "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                                         "clientInfo": {"name": "smoke", "version": "0"}}}, headers=hdr)
        check(st == 200 and d["result"]["protocolVersion"] == "2025-06-18", "mcp initialize",
              d["result"]["serverInfo"])
        st, _, _ = await call("POST", "/mcp", {"jsonrpc": "2.0", "method": "notifications/initialized"}, headers=hdr)
        check(st == 202, "mcp notification -> 202")
        st, d, _ = await call("POST", "/mcp", {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}, headers=hdr)
        names = [t["name"] for t in d["result"]["tools"]]
        check(names == ["measure", "configure", "get_reading", "get_state", "scpi"], "mcp tools/list", names)
        st, d, dt = await call("POST", "/mcp", {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                               "params": {"name": "measure", "arguments": {"function": "dcv", "samples": 2}}},
                               headers=hdr)
        r = d["result"]
        check(not r["isError"] and len(r["structuredContent"]["samples"]) == 2, "mcp measure",
              f"{dt:.2f}s {r['content'][0]['text'][:80]}")
        st, d, _ = await call("POST", "/mcp", {"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                              "params": {"name": "configure", "arguments": {"function": "ohms"}}}, headers=hdr)
        check(d["result"]["isError"], "mcp tool error is isError result", d["result"]["content"][0]["text"])
        st, d, _ = await call("POST", "/mcp", {"jsonrpc": "2.0", "id": 5, "method": "nope"}, headers=hdr)
        check(d["error"]["code"] == -32601, "mcp unknown method")
        st, _, _ = await call("GET", "/mcp")
        check(st == 405, "mcp GET -> 405")

        # ---- lead setting: the bridge refuses functions that do not suit the red lead's jack
        def mcp_call(i, name, args):
            return call("POST", "/mcp", {"jsonrpc": "2.0", "id": i, "method": "tools/call",
                                         "params": {"name": name, "arguments": args}}, headers=hdr)
        def mcp_body(d):
            return json.loads(d["result"]["content"][0]["text"])
        n0 = len(events)
        st, d, _ = await call("PUT", "/api/leads", {"red": "input"})
        check(st == 200 and d == {"red": "input"}, "leads set to input", d)
        await asyncio.sleep(0.3)
        check(any(e.get("type") == "leads" and e.get("red") == "input" for e in events[n0:]), "ws leads event")
        st, d, _ = await call("GET", "/api/state")
        check(d.get("leads") == "input", "state carries leads", d.get("leads"))
        st, d, _ = await call("PUT", "/api/config", {"function": "dci"})
        check(st == 409 and d["error"] == "leads_mismatch", "current refused with lead in Input", d.get("message"))
        st, d, _ = await call("GET", "/api/state")
        check(d["function"] == "dcv", "refusal leaves the meter alone", d["function"])
        st, d, _ = await mcp_call(6, "configure", {"function": "dci"})
        check(d["result"]["isError"] and mcp_body(d)["error"] == "leads_mismatch", "mcp gets leads_mismatch")
        st, d, _ = await mcp_call(7, "configure", {"function": "dcv", "force": True})
        check(d["result"]["isError"] and mcp_body(d)["error"] == "bad_request", "mcp cannot force", mcp_body(d)["message"])
        await call("PUT", "/api/leads", {"red": "ma"})
        st, d, _ = await call("PUT", "/api/config", {"function": "dcv"})
        check(st == 409 and d["error"] == "leads_mismatch", "volts refused with lead in mA jack", d.get("message"))
        st, d, dt = await call("PUT", "/api/config", {"function": "dci", "range": 0.05})
        check(st == 200 and d["state"]["function"] == "dci" and d["state"]["range"] == 0.05,
              "50 mA allowed from the mA jack", f"{dt:.2f}s")
        st, d, _ = await call("PUT", "/api/config", {"range": 10})
        check(st == 409 and d["error"] == "leads_mismatch", "10 A range refused from the mA jack", d.get("message"))
        await call("PUT", "/api/leads", {"red": "10a"})
        st, d, _ = await call("PUT", "/api/config", {"range": 0.05})
        check(st == 409 and d["error"] == "leads_mismatch", "50 mA range refused from the 10A jack", d.get("message"))
        st, d, _ = await call("PUT", "/api/config", {"range": 10})
        check(st == 200 and d["state"]["range"] == 10, "10 A range allowed from the 10A jack")
        st, d, dt = await call("PUT", "/api/config", {"function": "dcv", "force": True})
        check(st == 200 and d["state"]["function"] == "dcv", "force overrides the lead check", f"{dt:.2f}s")
        st, d, _ = await call("PUT", "/api/leads", {"red": "probe"})
        check(st == 400 and d["error"] == "bad_request", "rejects a bad lead value", d.get("message"))
        await call("PUT", "/api/leads", {"red": "none"})

        # ---- raw SCPI socket
        host = BASE.split("//")[1].split(":")[0]
        reader, writer = await asyncio.open_connection(host, 5025)
        writer.write(b"*IDN?\nMEAS?\n")
        idn = (await asyncio.wait_for(reader.readline(), 3)).decode().strip()
        meas = (await asyncio.wait_for(reader.readline(), 3)).decode().strip()
        writer.close()
        check(idn.startswith("Digcreat") and float(meas) is not None, "raw SCPI socket", f"{idn} | {meas}")

        # ---- WebSocket op
        await ws.send_json({"id": 99, "op": "reading", "fresh": True})
        t0 = time.monotonic()
        while not any(e.get("id") == 99 for e in events) and time.monotonic() - t0 < 5:
            await asyncio.sleep(0.05)
        reply = next((e for e in events if e.get("id") == 99), None)
        check(bool(reply and reply["ok"]), "ws op reading fresh", reply and reply.get("value"))
        n = len([e for e in events if e.get("type") == "reading"])
        check(n > 10, "ws streamed readings", n)

        collector.cancel()
        await ws.close()
        await call("PUT", "/api/leads", {"red": leads_before})

    failed = [n for ok, n in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    if failed:
        print("failed:", ", ".join(failed))
        sys.exit(1)


asyncio.run(main())
