# Digcreat XDM1041-DG: USB communication map

Findings from probing the meter on 2026-11-01 (macOS, unit plugged in over USB).

## Identity

```
*IDN?  ->  Digcreat,XDM1041 -DG,12345678,V4.4.0,3
```

Manufacturer `Digcreat`, model `XDM1041 -DG`, serial `12345678`, firmware `V4.4.0`.
This is a rebadged OWON XDM1041, so OWON's XDM SCPI documentation should mostly apply.

## Physical / link layer

| Item | Value |
|---|---|
| USB bridge chip | WCH CH340 (VID `0x1a86`, PID `0x7523`) |
| Class | Vendor-specific USB serial (not CDC-ACM) |
| macOS device | `/dev/cu.usbserial-1230` (built-in macOS CH34x driver, no install needed) |
| Serial settings | 115200 baud, 8N1, no flow control |
| Line terminator | Send `\n`; replies end with `\r\n` |

The USB port is just a UART behind a CH340. Any USB host that speaks CH34x can talk to it.

## Command behaviour

- Query latency is about 3 to 16 ms for supported commands.
- **Unsupported commands produce no reply at all** (no error string, `SYST:ERR?` is also
  unsupported). Every client must use a read timeout; about 600 ms is enough.
- `SYST:BEEP?` is supported but slow (about 500 ms).

## Supported queries (verified)

| Command | Example reply | Meaning |
|---|---|---|
| `*IDN?` | `Digcreat,XDM1041 -DG,12345678,V4.4.0,3` | Identity |
| `FUNC?` / `FUNC1?` | `"VOLT"` | Primary function |
| `FUNC2?` | `NONe` | Secondary display function |
| `MEAS?` / `MEAS1?` | `4.582938E-05` | Primary reading, SI units, scientific notation |
| `MEAS2?` | `NONe` | Secondary reading |
| `MEAS:SHOW?` | `0.0000VDC` | Reading formatted as shown on the display, with unit |
| `RANGE?` | `5 V` | Current range |
| `AUTO?` | `1` | Autorange on (1) / off (0) |
| `RATE?` | `S` | Sample rate: S (slow), M (medium), F (fast) |
| `SYST:BEEP?` | `OFF` | Beeper state |
| `TEMP:RTD:TYPE?` | `KITS90` | Thermocouple/RTD type |
| `TEMP:RTD:UNIT?` | `C` | Temperature unit |
| `CONT:THRE?` | `50` | Continuity threshold (ohms) |

## Not supported (no reply within 600 ms)

`READ?`, `FETC?`, `CONF?`, `RANG?`, `SYST:ERR?`, `SYST:VERS?`, `SYST:REM?`,
`CALC:FUNC?`, `CALC:STAT?`, `*STB?`, `*ESR?`, `*OPC?`

## Set commands (verified)

Set commands **never send a reply**, success or failure. Confirm with the matching query.

### Function select

| Command | `FUNC?` reply | Ranges seen in `RANGE?` |
|---|---|---|
| `CONF:VOLT:DC [range]` | `"VOLT"` | 50 mV, 500 mV, 5 V, 50 V, 500 V, 1000 V |
| `CONF:VOLT:AC [range]` | `"VOLT AC"` | 500 mV, 5 V, ... |
| `CONF:CURR:DC [range]` | `"CURR"` | 500 uA, 5 mA, ..., 10 A |
| `CONF:CURR:AC [range]` | `"CURR AC"` | 500 uA, ... |
| `CONF:RES [range]` | `"RES"` | 500 Ω, 5 KΩ, ..., 50 MΩ |
| `CONF:CAP` | `"CAP"` | 50 nF, ... |
| `CONF:FREQ` | `"FREQ"` | `Hz` (autorange reports 0) |
| `CONF:PER` | `"PER"` | `Hz` (autorange reports 0) |
| `CONF:DIOD` | `"DIOD"` | (empty) |
| `CONF:CONT` | `"CONT"` | (empty) |
| `CONF:TEMP` | `"TEMP"` | `KITS90` |

The optional `[range]` is the full-scale value in SI units, for example `CONF:VOLT:DC 0.5`,
`CONF:RES 5000`, `CONF:CURR:DC 0.005`. **It must be exactly one of the range values below**,
otherwise the command is silently ignored (`CONF:VOLT:DC 30` does nothing). The exception
is values above the top range, which select the top range (`CONF:VOLT:DC 750` gives 1000 V).
Giving a range turns autorange off. `CONF:VOLT:DC AUTO` turns autorange on but does a full
function reconfigure (about 450 ms busy); the plain `AUTO` command is much faster.

### Range tables (from a `RANGE 1..9` sweep)

`RANGE <n>` index to full-scale range. Indexes past the end select the top range.

| Function | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|
| VOLT:DC | 50 mV | 500 mV | 5 V | 50 V | 500 V | 1000 V | |
| VOLT:AC | 500 mV | 5 V | 50 V | 500 V | 750 V | | |
| CURR:DC | 500 uA | 5 mA | 50 mA | 500 mA | 5 A | 10 A | |
| CURR:AC | 500 uA | 5 mA | 50 mA | 500 mA | 5 A | 10 A | |
| RES | 500 Ω | 5 KΩ | 50 KΩ | 500 KΩ | 5 MΩ | 50 MΩ | |
| CAP | 50 nF | 500 nF | 5uF | 50uF | 500uF | 5 mF | 50 mF |

Note the inconsistent `RANGE?` formatting: capacitance ranges up to 500 uF have no space
(`5uF`); others do (`5 V`, `5 KΩ`). Parse the number and SI prefix, not a fixed format.

### Range, autorange, rate, beeper

| Command | Effect |
|---|---|
| `RANGE <n>` | Range by **index** (table above), not value. Turns autorange off. Value-style arguments (`RANGE 0.5`) are silently ignored. Busy only about 20 to 55 ms, and doesn't reconfigure the function. |
| `AUTO` | Autorange on. `RANGE?` keeps reporting the old range until the meter actually autoranges. |
| `RATE S` / `RATE M` / `RATE F` | Slow / medium / fast |
| `SYST:BEEP ON` / `OFF` | Beeper |

**Each function remembers its own range and autorange state.** For example, set 50 V manual in VDC,
switch to RES and back, and VDC is still 50 V manual. Don't assume `CONF:x` with no argument
gives autorange.

## Timing and pacing (important for the bridge firmware)

**The meter ignores commands it receives while it is busy, but not reliably.** Usually they are
dropped, but sometimes a query sent while busy is answered *late*. A late reply then looks
like the answer to your next query: in testing, an `AUTO?` probe's `1` was read as the reply to
`FUNC?` and to `MEAS?` (a phantom 1 V reading). After the meter answers, wait about 100 ms and
discard any further input before sending the next query. Also sanity-check reply formats:
`MEAS?` is always scientific notation, `FUNC?` is always a quoted string, and `RANGE?` always
has a unit or is empty.

Measured busy times (until it answers `AUTO?` again):

| Command | Busy time |
|---|---|
| `RATE x` (when the rate changes) | about 650 to 700 ms |
| `CONF:<function>` | about 490 ms |
| `SYST:BEEP x` | about 110 ms |
| `AUTO`, `RANGE n` | about 55 ms |

**Readings after a function or range change are invalid for about 2 to 2.8 s.** For about the
first 0.8 s after a function change `MEAS?` returns `0`. After that it returns the *previous raw
number rescaled into the new unit or range* (for example, 8.6 mV in VDC showed up as 8.6 kΩ after
switching to RES; 65 uV showed up as 65 nV after `RANGE 2`). Real readings start about 1.9 to 2.8 s
after the change. This applies to manual range changes too, and presumably to autorange steps.
The bridge treats a reading as real once the value changes at least 1.5 s after the change, or
after 3.5 s regardless. Rate changes don't produce stale readings.

**Reading update rate is low, and the serial link is not the bottleneck.** About 170 `MEAS?`
queries/s are possible, but new values arrive only at:

| `RATE` | New readings/s |
|---|---|
| F | about 3 |
| M | about 1.5 |
| S | about 1 |

Repeated `MEAS?` calls between updates return the same cached value.

## Special values and encoding

- Overload or open input: `MEAS?` returns `1E+9`, and `MEAS:SHOW?` returns `overload` (or `open`
  for diode and continuity).
- **The Ω character is encoded two different ways:** `RANGE?` sends UTF-8 (`CE A9`), and
  `MEAS:SHOW?` sends GB2312 (`A6 B8`). Decode `MEAS:SHOW?` as GB2312, or better, use
  `MEAS?` (plain ASCII) plus `FUNC?` for the unit.

## Still to map

- Confirm stale-reading behaviour during autorange steps with a real signal.

- Secondary display (`FUNC2`/`MEAS2` set commands, for example frequency alongside VAC).
- Math/relative/dBm modes, temperature sensor type set commands.
- Remote/local lockout behaviour with the front panel.
