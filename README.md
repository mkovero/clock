# aika — rubidium frequency and GNSS time reference

A home-built timing rig. An AccuBeat AR-40A rubidium standard supplies 10 MHz for audio
and bench clocking and also clocks a Raspberry Pi CM4 ("aika"). A u-blox ZED-F9T timing
receiver ties that clock to UTC, and aika serves the result as a stratum-1 NTP server and
a PTP grandmaster.

Live status: <https://www.mui.fi/clock/>

## What it is for

One rubidium serves two separate goals:

1. **Frequency reference.** Short-term stability and holdover for the Antelope Orion 32 HD
   10M input and bench instruments.
2. **Time reference.** UTC-traceable time on the CM4. chrony serves it over NTP and
   ptp4l serves it over PTP using the PHY's hardware timestamps.

The rubidium supplies stability and the ZED-F9T supplies traceability. The AR-40A is
never electrically steered: whatever offset it has stays on the raw 10 MHz, and chrony
corrects it in the system clock against GNSS PPS. It was trimmed mechanically on
2026-09-21 from −2.55×10⁻⁹ to +2.2×10⁻¹¹ ([log](config/ar40a-trim-log.txt)), which is a
state rather than a property: a month later it reads +5.7×10⁻¹¹, the drift its specified
aging of 5×10⁻¹⁰/yr allows. Trimming buys nothing for timekeeping, because chrony carries a
static offset regardless; it matters only to whatever reads the raw 10 MHz.

Acting as a Dante or AES67 grandmaster is not a goal.

## Architecture

```
AccuBeat AR-40A rubidium ── 10 MHz sine
        │
   7 dB pi pad
        │ 1.245 Vpp
   Extron DA (R plane) ─┬─► Orion 32 HD 10M input
                        ├─► bench instruments (planned)
                        │
                        ▼
   Si5351C ◄── I2C ── Teensy 4.0 ◄── AR-40A BIT (lock)
   10 MHz × 86.4 ÷ 16    │ gates CLK4, drives panel LEDs
        │ 54 MHz         │ status text (ttyAMA5)
        ▼                ▼
   ┌───────────────────────────────────────────┐
   │ Raspberry Pi CM4 "aika"                   │
   │  XIN ← 54 MHz (crystal removed)           │
   │  PHY PHC /dev/ptp0 ← PPS on J2 pin 9      │ eth0
   │  ttyAMA0 ← UBX/NMEA, 460800               ├────► NTP + PTP
   │  chrony · gpsd · ptp4l · ts2phc           │
   └───────────────────────────────────────────┘
        ▲ TIME1 1 PPS        ▲ UART1
        └──── u-blox ZED-F9T ┘
                  │ 8 m coax
             GNSS antenna
```

**Frequency path.** A pad brings the AR-40A down to the Extron distribution amplifier's
input range. Each destination gets its own isolated DA output. The Si5351C multiplies
10 MHz up to 54 MHz, and that signal replaces the CM4's crystal, so the whole SoC runs on
the rubidium. The Teensy switches CLK4 off unless the Si5351 reports a valid reference
and PLL lock. Without that, a missing reference produces a stable ~11.6 MHz that the CM4
cannot boot from.

**Time path.** The F9T runs in fixed-position timing mode at a PPP position (RTKLIB with
ESA final products, since 2026-10-09). Its TIME1 pulse, the output whose quantisation error
`UBX-TIM-TP` reports, is timestamped by the CM4's BCM54210PE PHY. chrony reads that timestamp as
its `PPS2` refclock and uses gpsd's NMEA only to label which second each pulse belongs
to. The same pulse events also discipline the PHC directly through ts2phc, which ptp4l serves
as grandmaster (sd 6 ns, against 137 ns when the PHC was copied from the system clock by
phc2sys). Three MIKES NTP servers act as a millisecond-level sanity check.

**Power.** A MeanWell RS-15-5 feeds three separate 5 V runs (CM4, Si5351 + Teensy, F9T).
The AR-40A has its own 15 V supply, and its negative rail is deliberately not tied to
the 5 V one.

## Typical performance

Week of 2026-10-16 to 10-23, from the dashboard history (median, 5–95 % in brackets). The
dashboard has live values.

| Quantity | Value |
|---|---|
| chrony | stratum 1, `PPS2` selected throughout |
| PPS2 jitter (chrony std dev) | 11 ns (10–14) |
| Receiver time accuracy (`tAcc`) | 1 ns (1–2) |
| Satellites | 19 used (15–23) / 37 seen (33–41), GPS, Galileo, BeiDou; PDOP 0.97 |
| chrony max-error bound | 32 µs (25–40) |
| AR-40A offset vs GNSS | +5.7×10⁻¹¹ (4.4–6.9), skew 1.7×10⁻¹¹ |
| Frequency resolution of the rig | a few ×10⁻¹² over hours |

**Absolute time.** Against GPS time as an independent PPP solution realises it, the pulse
sits at **PPS − GPS = −2.1 ns, sd 3.5 ns** (2026-10-19/20 session, ESA rapid products;
[how](docs/pulse-to-gps-tie.md)). That excludes the antenna, LNA and cable delay, which is
unmeasured and larger than the tie itself. See
[docs/timing.md](docs/timing.md#bias-and-error-budget).

**The antenna sees about half the sky.** Azimuths from N clockwise to SSE return almost no
carrier phase at any elevation, and something overhead costs 15 dB above 75°. It is
structure, not the mount, and it is why PPP cannot resolve ambiguities here
([hardware](docs/hardware.md#what-the-antenna-can-actually-see)).

## Repository

| Path | Contents |
|---|---|
| `5351.ino` | Teensy 4.0 firmware: Si5351C setup, CLK4 gating, status reporting |
| `tools/` | CM4-side scripts: `gpsstat` health check, receiver config (`ubx-apply-config`, `ubx-dump-config`, `f9t-restore`), survey and raw logging (`f9t-survey`, `f9t-rawlog`), dashboard collector, `f9t-skymap`, `rb-freq-live`, `rb-adev`, `qerr-vs-extts` |
| | PPP and time-transfer tools, run on a workstation: `f9t-ppp` (UBX to RINEX), `rinex-filter` (constellation and C/N0 filter before CSRS), `ubx-timtp` (pulse quantisation error), `ubx-rxclock` (receiver clock bias), `ppp-clk-adev`, `ppp-vs-utcmike` (pulse against GPS and UTC(MIKE) via the BIPM's public files) |
| `dashboard/` | static status page and its deployment notes |
| `config/` | ZED-F9T applied changes, `chrony.conf`, the AR-40A trim log, dated reminders (`f9t-reminders` for gpsstat, `milestones.json` for the dashboard). Anything carrying the position is kept private ([why](config/README.md)) |
| `plan.md` | open work, next steps, and things deliberately not being done |

## Documentation

- [docs/hardware.md](docs/hardware.md) — every component, its wiring, levels, and
  what is out of spec on purpose
- [docs/grounding.md](docs/grounding.md) — ground and earth topology, rules, and
  measurements
- [docs/timing.md](docs/timing.md) — OS, chrony/gpsd/PTP, receiver configuration,
  error budget, DGNSS vs PPP, tools
- [docs/pulse-to-gps-tie.md](docs/pulse-to-gps-tie.md) — tying the pulse to GPS time
  through PPP, why one CSRS position was 8.5 m off, and the current tie
- [docs/troubleshooting.md](docs/troubleshooting.md) — failure modes and traps
  that have already cost time, with recovery steps
- [docs/timing-board.md](docs/timing-board.md) — design notes for the planned
  squarer/divider board
- [dashboard/README.md](dashboard/README.md) — dashboard data, deployment, and how
  to read its numbers
- [plan.md](plan.md) — to-do list
