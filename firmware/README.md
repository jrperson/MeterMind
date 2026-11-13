# MeterMind firmware (ESP32-S3)

ESP-IDF 5.5 firmware (5.5.2 or later; releases use 5.5.4) for the MeterMind board (v0.1, ESP32-S3-MINI-1-N8). It is a
port of the Python prototype in `../bridge/` and serves the same API (`../API.md`) on port 80:
REST, WebSocket `/ws`, MCP `/mcp`, the dashboard at `/`, and raw SCPI on 5025.

| File | Port of | What it does |
|---|---|---|
| `main/meter.c` | `meter.py` | One task owns the USB link: job queue, 10 Hz polling, pacing, verify, settle |
| `main/usb_link.c` | pyserial | USB host to the meter's CH340 (1a86:7523), line reads with timeouts |
| `main/proto.c` | tables in `meter.py` | Function/range tables, reply parsing, GB2312 Ω (host-tested) |
| `main/bridge.c` | `api.py` | Operations shared by every transport, validation, error codes |
| `main/mcp.c` | `mcp.py` | MCP JSON-RPC (Streamable HTTP, stateless, JSON only) |
| `main/http.c`, `main/scpi_server.c` | `server.py` | HTTP/WebSocket/MCP on one port; raw SCPI over TCP |
| `main/net.c` | `--mdns` | WiFi station, setup access point, mDNS (`metermind.local`), SNTP |
| `main/portal.c`, `main/captive_dns.c`, `main/setup.html` | | Setup page, `/api/wifi`, captive-portal DNS and redirect |
| `main/board.c`, `main/console.c` | | VBUS switch, status LED, BOOT button, serial console |

## Build and flash

```sh
. ~/esp/esp-idf-v5.5/export.sh
cd firmware
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor   # the board's USB-C port (CH340C, auto-reset)
```

`../tools/build_flasher.sh` makes the release build (from `sdkconfig.defaults` only) and assembles the
web installer in `../flasher/`, which the release workflow publishes to GitHub Pages.

`tests/run_tests.sh` builds `main/proto.c` on the host computer and checks it against the prototype's parser.

## WiFi setup

With no network saved (first boot, or after `wifi clear`), the board opens its own WiFi network,
**MeterMind-XXXX** (the last four hex digits of its MAC), and the LED blinks blue.

1. Join MeterMind-XXXX from a phone or laptop. Most phones open the setup page by themselves
   (captive portal); otherwise browse to `http://192.168.4.1/`.
2. Pick your network (2.4 GHz only), type its password and press **Join network**.
3. The board tries it while the setup network stays up. A wrong password or a missing network is
   reported on the page; nothing is saved until the board has an address.
4. On success the page shows the address. Rejoin your own network and open
   `http://metermind.local/`. The setup network closes 60 s later, or at once with **Finish**.

To change the network later, **hold BOOT for 3 s** (the LED turns white, then blinks blue): the
setup network opens again and the board leaves your network meanwhile. Holding BOOT for 3 s again,
or **Finish**, closes it and rejoins the saved network; so does nobody joining it for 10 minutes
(`CONFIG_BRIDGE_SETUP_TIMEOUT_MIN`). Without a saved network it stays open.

The setup network is open by default. Set `CONFIG_BRIDGE_SETUP_PASSWORD` (8 to 63 characters,
`idf.py menuconfig` > MeterMind) to make it WPA2. The `/api/wifi` endpoints that change anything
answer only in setup mode, so nobody on your LAN can move the board to another network.

The serial console (`idf.py monitor`, 115200) does the same without a phone:

```
> wifi "My Network" "my password"     # saves at once and joins (leaves setup mode)
> setup                               # opens the setup network; "setup off" closes it
> status
```

Credentials are stored in NVS, not in the image. Other commands: `wifi clear` (erase and restart
into setup mode), `vbus cycle`, `reboot`, `help`.

## Status LED

| Colour | Meaning |
|---|---|
| blue, blinking | setup mode: join MeterMind-XXXX |
| yellow, blinking | setup mode, trying the network chosen on the setup page |
| yellow | connecting to WiFi |
| orange | on WiFi, meter not connected or not answering |
| green, cyan blink per reading | running |
| red, blinking | meter VBUS overcurrent (U3 fault) |
| white | BOOT held (3 s toggles setup mode) |

## Behaviour beyond the prototype

- The meter's VBUS comes from U3 (TPS2051C, 0.5 A). If the USB link is up but the meter has not
  answered for 30 s, the firmware switches VBUS off for 1 s to reset its CH340 (at most every 5 min).
- `/api/wifi` (status, scan, join, done) and the setup page; see `../API.md`.
- `GET /api/info` adds `meter_vbus_fault`. Readings carry `ts` (unix seconds) once SNTP has set
  the clock.
- Two 3.5 MB OTA slots are reserved (`partitions.csv`); OTA itself is not written yet.

## Testing on the board

With the meter plugged in and the probes disconnected, the prototype's end-to-end test runs
against the board unchanged: `python tools/smoke_test.py http://metermind.local`.
