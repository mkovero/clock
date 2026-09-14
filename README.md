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
never steered. Its frequency offset (about −2.56×10⁻⁹) stays on the raw 10 MHz, and chrony
corrects it in the system clock against GNSS PPS.

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
   │  ttyAMA0 ← UBX/NMEA, 115200               ├────► NTP + PTP
   │  chrony · gpsd · ptp4l · phc2sys          │
   └───────────────────────────────────────────┘
        ▲ TIME2 1 PPS        ▲ UART1
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

**Time path.** The F9T runs in fixed-position timing mode at a PPP-surveyed position. Its
TIME2 pulse is timestamped by the CM4's BCM54210PE PHY. chrony reads that timestamp as
its `PPS2` refclock and uses gpsd's NMEA only to label which second each pulse belongs
to. phc2sys copies the disciplined system clock back into the PHC for ptp4l. Three MIKES
NTP servers act as a millisecond-level sanity check.

**Power.** A MeanWell RS-15-5 feeds three separate 5 V runs (CM4, Si5351 + Teensy, F9T).
The AR-40A has its own 15 V supply, and its negative rail is deliberately not tied to
the 5 V one.

## Typical performance

Checked 2026-09-14. The dashboard has live values.

| Quantity | Value |
|---|---|
| chrony | stratum 1, `PPS2` selected |
| PPS2 jitter (chrony std dev) | 20–60 ns |
| Receiver time accuracy (`tAcc`) | 1 ns |
| Satellites used | ~50 (GPS, Galileo, BeiDou), PDOP ≈ 1 |
| chrony max-error bound | ~41 µs |
| AR-40A offset vs GNSS | −2.56×10⁻⁹ |

These figures describe jitter and servo quality, not absolute UTC bias. Fixed antenna
LNA and receiver delays are still unmeasured. See [docs/timing.md](docs/timing.md#bias-and-error-budget).

## Repository

| Path | Contents |
|---|---|
| `5351.ino` | Teensy 4.0 firmware: Si5351C setup, CLK4 gating, status reporting |
| `tools/` | CM4-side scripts: `gpsstat` health check, receiver config, survey/PPP, dashboard collector |
| `dashboard/` | static status page and its deployment notes |
| `config/` | ZED-F9T config backups, applied changes, survey and PPP results, dated reminders |
| `plan.md` | open work, next steps, and things deliberately not being done |

## Documentation

- [docs/hardware.md](docs/hardware.md) — every component, its wiring, levels, and
  what is out of spec on purpose
- [docs/grounding.md](docs/grounding.md) — ground and earth topology, rules, and
  measurements
- [docs/timing.md](docs/timing.md) — OS, chrony/gpsd/PTP, receiver configuration,
  error budget, DGNSS vs PPP, tools
- [docs/troubleshooting.md](docs/troubleshooting.md) — failure modes and traps
  that have already cost time, with recovery steps
- [docs/timing-board.md](docs/timing-board.md) — design notes for the planned
  squarer/divider board
- [dashboard/README.md](dashboard/README.md) — dashboard data, deployment, and how
  to read its numbers
- [plan.md](plan.md) — to-do list
