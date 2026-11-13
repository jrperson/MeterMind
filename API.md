# MeterMind: network API (v0.2)

The ESP32 owns the serial link and hides every meter quirk listed in `PROTOCOL.md`
(silent set commands, dropped commands while busy, invalid readings after a function change,
inconsistent Ω encoding). Clients speak plain HTTP + JSON, or WebSocket for streaming.
No client library is required: `curl`, a browser, or any language's HTTP client works.

Discovery: mDNS hostname `metermind.local`, services `_metermind._tcp`, `_http._tcp`, `_scpi-raw._tcp`.

Ports: the ESP32 serves HTTP on 80. The Python prototype defaults to **8041** (`--http-port`).
Raw SCPI is on 5025 in both. Implemented in `bridge/` (prototype) and `firmware/` (ESP32);
`tools/smoke_test.py` exercises all of it against either.

## Conventions

- Function names: `dcv`, `acv`, `dci`, `aci`, `res`, `cap`, `freq`, `period`, `diode`,
  `continuity`, `temp`.
- Values are always SI floats with a separate `unit` (`V`, `A`, `ohm`, `F`, `Hz`, `s`, `C`).
- Overload/open is `"value": null, "overload": true`, never the meter's raw `1E+9`.
- Ranges are given in real units (`5`, `0.5`, `50000`) or `"auto"`. The bridge rounds up to the
  next real range (`30` on dcv selects 50 V) and reports the range it selected. A value above the
  top range is a `bad_request` listing the valid ranges.
- **Giving a function without a range means autorange.** The meter itself silently remembers the
  last manual range per function, which is a trap for automated clients, so the bridge doesn't
  expose that behaviour. Changing only `range`, `rate` or `beep` leaves the function alone.
- Unknown fields are rejected (`bad_request`), so typos fail loudly.
- Errors: HTTP 4xx/5xx with `{"error": "<code>", "message": "..."}`.
  Codes and statuses: `bad_request` 400, `forbidden` 403 (cross-origin), `busy` 409,
  `verify_failed` 502, `meter_disconnected` 503, `meter_timeout` 504.

## HTTP endpoints

### `GET /api/info`
```json
{"model":"XDM1041 -DG","serial":"12345678","meter_fw":"V4.4.0","bridge_fw":"0.2.0",
 "connected":true,"uptime_s":5123}
```
The ESP32 firmware adds `"meter_vbus_fault": false` (the meter's 0.5 A USB power switch has tripped).

### `GET /api/state`
Current configuration as last verified with the meter. Front-panel changes are picked up
within about 1 s.
```json
{"function":"dcv","range":5,"range_label":"5 V","autorange":true,"rate":"slow",
 "beep":false,"settled":true}
```

### `PUT /api/config`
Change any subset of settings. The bridge sends the commands with correct pacing, re-queries the
meter to verify every field, waits until readings are valid, then responds. One call, one
trustworthy answer.
```json
// request
{"function":"res","range":"auto","rate":"fast"}
// response (after about 3 s for a function change)
{"state":{"function":"res","range":5000,"range_label":"5 KΩ","autorange":true,
          "rate":"fast","beep":false,"settled":true},
 "reading":{"value":1002.3,"unit":"ohm","overload":false,"seq":812,"t_ms":5123456}}
```
Fields that already match are skipped (no needless 3 s settle).

Autorange can take longer: each range step settles again (about 1.8 s per step), so resistance on
an open input that starts from 500 Ω answers after about 11 s. The bridge waits as long as the
meter keeps stepping, up to 19.5 s in all, before `504 meter_timeout`.

### `GET /api/reading`
Latest valid reading. Returns `409 busy` with `retry_after_ms` while settling.
- `?fresh=1`: wait for a reading taken after this request arrived.
- `?timeout_ms=...`: upper bound on waiting (default 5000).
```json
{"value":3.30112,"unit":"V","function":"dcv","range_label":"5 V","overload":false,
 "display":"3.3011VDC","seq":813,"t_ms":5124460,"age_ms":120}
```
`t_ms` counts from bridge start. Readings also carry `ts` (unix time in seconds) when the bridge's
clock is set; the ESP32 sets it by SNTP and omits `ts` until then.

### `POST /api/measure`
The single call most AI projects need: configure if needed, wait for settle, collect N fresh
samples, return stats. Without `timeout_ms` the wait follows autorange steps as for
`PUT /api/config` (up to 12 s beyond the normal wait); an explicit `timeout_ms` is a hard limit.
```json
// request
{"function":"dcv","range":"auto","samples":5}
// response
{"function":"dcv","unit":"V","samples":[3.3011,3.3012,3.3011,3.3010,3.3012],
 "mean":3.30112,"min":3.3010,"max":3.3012,"stdev":0.00008,"overload":false,
 "duration_ms":4900}
```

### `POST /api/scpi`
Escape hatch for unmapped features. Still goes through the bridge's command queue, so it
can't collide with polling.
```json
{"cmd":"CONT:THRE?"}          ->  {"reply":"50"}
{"cmd":"SYST:BEEP ON"}        ->  {"reply":null}
```

### `GET /api/leads`, `PUT /api/leads`
The meter cannot sense where the test leads are, so the user tells the bridge which jack the red
lead is in (the black lead is always in COM). The setting is shared by every client and is
`unknown` after the bridge starts.
```json
{"red":"ma"}    // one of: unknown, none (no leads), input, ma (the µA mA jack), 10a
```
Every state object carries it as `leads`, the WebSocket `hello` carries `{"leads":{"red":...}}`,
and a change is broadcast as `{"type":"leads","red":"ma"}`.

`PUT /api/config` and `POST /api/measure` refuse a function or range change that does not suit
the jack with `409 {"error":"leads_mismatch","message":...,"leads":"input","function":"dci"}`:

| Red lead | Refused |
|---|---|
| `unknown` | `dci`, `aci` (set the leads first) |
| `input` | `dci`, `aci` |
| `ma` | every function except `dci`/`aci`; the 5 A and 10 A ranges |
| `10a` | every function except `dci`/`aci`; the 500 µA to 500 mA ranges |
| `none` | nothing |

A function in a current jack is refused because it puts the input across the current shunt,
which blows the fuse when the leads touch a voltage source. Requests that change neither function
nor range (rate, beep, sampling in the current mode) are not checked. Add `"force": true` to the
request to skip the check; the dashboard does this only after the user confirms. MCP tools do not
accept `force`: an agent that gets `leads_mismatch` has to ask the user.

### WiFi setup (firmware only)
The board's own setup network (MeterMind-XXXX, `http://192.168.4.1/`) opens when no network is
saved or after BOOT is held for 3 s. Everything except `GET /api/wifi` answers
`409 {"error":"setup_required"}` outside setup mode.

- `GET /api/wifi`: `{setup, configured, state ("idle"|"connecting"|"up"), ssid, ip, rssi,
  hostname, ap_ssid, ap_ip, close_in_s, attempt}`. `attempt` is null or
  `{ssid, state ("trying"|"ok"|"failed"), message}`; the password is never returned.
- `GET /api/wifi/scan?refresh=1`: `{scanning, networks: [{ssid, rssi, secure}]}`, strongest
  first. Poll without `refresh` until `scanning` is false.
- `POST /api/wifi` `{"ssid":"Home","password":"..."}` -> `202`. The board tries the network and
  saves it only once it has an address; follow `attempt` in `GET /api/wifi`. An empty or absent
  password means an open network.
- `POST /api/wifi/done`: close the setup network and rejoin the saved one.

In setup mode `/` serves the setup page (also at `/setup`), every DNS name resolves to the board
and unknown paths redirect to it, so phones show the page as a captive portal.

## WebSocket `/ws`

Server pushes events to every client:
```json
{"type":"reading","value":3.3011,"unit":"V","function":"dcv","overload":false,"seq":814,"t_ms":5125460}
{"type":"state","function":"res","range_label":"500 Ω","autorange":true,"rate":"fast","settled":false}
{"type":"status","connected":false}
```
Clients can send the same operations as the HTTP API, with an `id` to match replies:
```json
{"id":7,"op":"config","function":"acv"}   ->  {"id":7,"ok":true,"state":{...},"reading":{...}}
{"id":8,"op":"measure","samples":3}       ->  {"id":8,"ok":true,"mean":...}
```

## Optional: raw SCPI on TCP 5025

For PyVISA and existing instrument software. Routed through the same queue and pacing.

## Bridge behaviour (what the firmware guarantees)

- One task owns the serial port. All requests from all clients go through one queue.
- After each set command the bridge probes with `AUTO?` until the meter answers, then
  discards any late replies before the next query.
- Every set is verified by query; one automatic retry, then `verify_failed`.
- After a function change, a range change, or an autorange step (front panel included),
  readings are suppressed until the value changes at least 1.5 s later, or 3.5 s pass.
  A config or measure call waiting for its reading gets a fresh allowance at each such step,
  capped at 12 s beyond its normal wait.
- Background polling at 10 Hz for `MEAS?`, `RANGE?` and `MEAS:SHOW?`, 1 Hz for function,
  autorange and rate. A new `seq` is issued
  per meter update interval (about 1 s slow, 0.67 s medium, 0.33 s fast).
- Multiple clients: last writer wins, and every change is broadcast as a `state` event.

## MCP endpoint `/mcp`

MCP Streamable HTTP transport, stateless, JSON responses (no SSE; `GET`/`DELETE` return 405).
Verified with the official MCP Python SDK client (negotiates protocol 2025-11-25; also accepts
2025-06-18, 2025-03-26, 2024-11-05).

| Tool | Arguments | Returns |
|---|---|---|
| `measure` | `function?`, `range?`, `rate?`, `samples?` (1 to 100) | same as `POST /api/measure` |
| `configure` | `function?`, `range?`, `rate?`, `beep?` | same as `PUT /api/config` |
| `get_reading` | `fresh?` | same as `GET /api/reading` |
| `get_state` | none | `{info, state}` |
| `scpi` | `command` | `{reply}` |

Successful results carry both `structuredContent` and a JSON text block. Errors come back as tool
results with `isError: true` and the same `{error, message, ...}` body as the HTTP API in the text
block only, so the agent can read and react to them. The server `instructions` tell the agent that the bridge can't see
the leads, that `get_state` reports where the user says the red lead is (`state.leads`), and that
on `leads_mismatch` it must ask the user to move the lead and update the setting rather than work
around it. `measure` and `configure` refuse `force`.

Add to Claude Code: `claude mcp add --transport http metermind http://<bridge-host>:<port>/mcp`

## Security

Requests carrying an `Origin` header are refused (403) unless it matches the bridge's own host
or localhost. This blocks DNS-rebinding attacks from web pages (required by the MCP spec).
Clients that send no `Origin` (curl, scripts, agents) are unaffected. There is no
authentication yet: anyone on the LAN can drive the meter.

## Later

- OTA firmware updates, optional client lock
  (`POST /api/lock`) for long automated runs.
