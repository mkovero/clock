# 10 MHz Reference & Timing Rig — Architecture

Status as of 2026-09-07. Written from a rig that was already running; this documents
what exists, what is out of spec, and what is still unmeasured.

**Confidence markers used throughout:**
- `[built]` — how it is actually wired, per direct account
- `[spec]` — from a datasheet or manual, not verified on this hardware
- `[calc]` — derived number, follows from `[spec]` values
- `[?]` — unverified, needs a measurement
- `[plan]` — not built yet

---

## 1. Purpose

Two independent goals sharing one rubidium:

1. **Frequency reference.** Short-term stability and holdover for audio clocking
   (Orion 32 HD) and for bench instruments.
2. **Time reference.** UTC-traceable absolute time on the CM4, which runs chrony
   as a stratum-1 NTP/PTP source.

These are complementary, not redundant: the AR-40A supplies stability, the ZED-F9T
supplies traceability. Effectively a GPSDO architecture split across separate boxes,
with the disciplining loop currently open (see §7).

Explicitly **not** a goal: acting as a Dante or AES67 grandmaster.

---

## 2. Block diagram (as built)

```
                    AccuBeat AR-40A
                  10 MHz sine, +12±2 dBm
                           │
                           │  SMA
                           │
                     ┌─────┴─────┐   passive T, no pad
                     │           │
                     ▼           ▼
        Extron DA RGB/YUV   Si5351C breakout
        (75 Ω video DA)      CLKIN
                     │           │
          ┌──────────┤           │ Teensy 3.2 ── I2C (pins 18/19)
          │          │           │
          ▼          ▼           ▼
    Orion 32 HD   bench       CLK4, 8 mA
    10M input    instruments  54 MHz
    (sine)       (50 Ω)          │
                                 │ coax + 10 Ω series damping
                                 │ (grounded both ends —
                                 │  sole bond between islands)
                                 ▼
                        ┌────────────────┐
                        │  RPi CM4       │  BCM2711 XIN
                        │  realtime Linux│  (crystal removed)
                        │  chrony        │
                        └────────────────┘
                             ▲       ▲
                     1 PPS ──┘       └── USB (D+/D- on
                  (PTP hw irq)            breakout header)
                             │              │
                        ┌────┴──────────────┴───┐
                        │  u-blox ZED-F9T       │
                        │  breakout             │
                        └───────────────────────┘
                                   │ SMA
                                   ▼
                              GNSS antenna
```

---

## 3. Component inventory

### AccuBeat AR-40A rubidium frequency standard
| Parameter | Value | Src |
|---|---|---|
| Output | 10 MHz sine, +12±2 dBm into 50 Ω | `[spec]` |
| → into 50 Ω load | ~2.5 Vpp | `[calc]` |
| → open-circuit EMF | ~5.04 Vpp | `[calc]` |
| Aging | <1×10⁻⁹ first year, <5×10⁻¹⁰/yr after | `[spec]` |
| Allan deviation | <3×10⁻¹¹ @ 1 s, <3×10⁻¹² @ 100 s | `[spec]` |
| Stability vs temp | ±2×10⁻¹⁰ over −5…+50 °C | `[spec]` |
| Accuracy at shipment | 5×10⁻¹¹ | `[spec]` |
| Supply | 15 Vdc ±5%, ~0.6 A steady, ~1.7 A warm-up | `[spec]` |
| Warm-up | 5 min to lock, 7.5 min to 5×10⁻¹⁰ | `[spec]` |
| Mech. trim | trimmer under calibration sticker, 1 turn ≈ 5×10⁻¹⁰, 10 turns | `[spec]` |
| BIT | DB9 pin 3, open-collector | `[spec]` |

**Source impedance is assumed, not specified.** The manual states delivered power
into a 50 Ω load; it does not state that the output *is* a 50 Ω source. The EMF and
into-75 Ω figures above assume it is. If the output is instead a low-impedance
buffer, it delivers ~2.5 Vpp largely independent of load, and the level at the
Extron is ~2.5 Vpp rather than 3.02 — about 1.6 dB apart, half a pad step.

Settle both at once with a two-point measurement: Vpp open-circuit, then Vpp across
a known 50 Ω terminator. Loaded ≈ half of open → 50 Ω source, tables above apply.
Loaded ≈ open → buffered output, size the pad from the loaded figure directly. `[?]`

**Manual inconsistency to resolve.** The AR-40A manual states BIT polarity two ways
that contradict each other:

- §1.3 spec table and §2.1.2: locked = pin 3 shorted to ground ("0")
- §3.3: "0" logic described as *open collector* = lock; "1" = *short to ground* = unlock

The physical description in §2.1.2 (open collector, pulled to ground when locked, i.e.
LED on = locked, matching the §2.4 turn-on step "BIT LED should be illuminated after
about 4 minutes") is the self-consistent reading. §3.3 appears to have the labels
swapped. **Verify by observation:** watch the pin from cold start through lock. `[?]`

### Extron DA RGB/YUV distribution amplifier
| Parameter | Value | Src |
|---|---|---|
| Input impedance | 75 Ω, return loss <−38 dB @ 5 MHz | `[spec]` |
| Input level | 0.3–1.5 Vpp, **1.5 Vpp maximum** at unity gain | `[spec]` |
| Bandwidth | 350 MHz (−3 dB), fully loaded | `[spec]` |
| Gain | 0 dB (unity) or +1.1 dB, DIP-selectable | `[spec]` |
| Peaking | 0 dB or +6 dB @ 100 MHz, DIP-selectable | `[spec]` |
| Output | 75 Ω series build-out; unity only into terminated 75 Ω | `[spec]` |
| Output DC offset | ±5 mV | `[spec]` |
| Coupling | AC/DC switch-selectable | `[spec]` |

**Usage constraints:**
- Use **R, G or B** planes (or Y on a YUV A). The H/V sync inputs are 510 Ω into a
  TTL squarer spec'd 15–180 kHz — useless at 10 MHz.
- On a DA6 YUV A, avoid the digital audio BNC: 510 Ω input, 2.5 Vpp output.
- DIP switches: Gain/Peak **off** (no shaping on a reference sine). AC coupling is
  the safer default.
- R, G and B are independent signal paths sharing one chassis, so a 6-output unit
  fed on R leaves G and B as two more idle 1→6 buses.

### Antelope Orion 32 HD — 10M input
| Parameter | Value | Src |
|---|---|---|
| Signal | sine | `[built]` |
| Impedance | 75 Ω | `[spec]` |
| Level | 1 Vpp nominal | `[spec]` |
| Front end | comparator / squarer, self-biasing | `[spec]` |
| Actual AC termination | unconfirmed | `[?]` |

Note: every Orion clock mode disciplines the same internal OCXO. The advantage of
10M over word clock is the multiplication ratio (~×2.26 vs ×512), i.e. far less
phase-noise amplification. 10M is the right input to use.

### Si5351C-B breakout + Teensy 3.2
| Parameter | Value | Src |
|---|---|---|
| CLKIN V_IL | max 0.3 × VDD (0.99 V @ 3.3 V) | `[spec]` |
| CLKIN V_IH | min 0.7 × VDD (2.31 V @ 3.3 V) | `[spec]` |
| CLKIN abs max | **−0.5 to +3.8 V** | `[spec]` |
| CLKIN frequency | 10–100 MHz; internal divider limits PLL input to 30 MHz | `[spec]` |
| CLKIN bias | **none internal** — high-Z, needs external bias | `[spec]` |
| Output period jitter | <70 ps pp typ | `[spec]` |
| Config | PLLA from CLKIN, 10 MHz × 86.4 = 864 MHz VCO; MS4 ÷16 → 54 MHz on CLK4 | `[built]` |
| CLK4 drive | 8 mA, 10 Ω series damping resistor | `[built]` |
| Teensy I2C | default pins 18 (SDA) / 19 (SCL) | `[built]` |

**Do not** feed 10 MHz into XA — that input is specified 25–27 MHz only.

### Raspberry Pi CM4
- BCM2711 crystal removed; 54 MHz injected at XIN from Si5351 CLK4. `[built]`
- Out of spec on paper (crystal inputs generally want ~1 Vpp AC-coupled; a 10 Ω
  series resistor into a high-Z XIN passes essentially the full 3.3 V CMOS swing),
  but stable in service for years. `[built]`
- Dedicated realtime Linux. `[built]`
- chrony, stratum-1 NTP/PTP source. `[built]`

### u-blox ZED-F9T on breakout
- I/O 3.3 V at the module. Whether *this breakout* level-shifts to 5 V is
  unconfirmed. `[?]`
- Header: +5V, GND, RX(2)/TX(2), TIME(1)/TIME(2), EXTINT, READY, SCL/SPI_CLK,
  SDA/SPI_CS, USB D−/D+, RX/SPI_MOSI, TX/SPI_MISO. SEL pad selects UART+I²C vs SPI.
- TIME(1) = TIMEPULSE1 = 1 PPS default → CM4 PTP hardware interrupt. `[built]`
- Data to CM4 over USB, ordinary USB cable on the D+/D− header pins. Worked, but
  grounding was sketchy in several places. `[built]`
- EXTINT unused. Available for UBX-TIM-TM2 time-marking. `[plan]`

---

## 4. Levels

Everything downstream is set by one number: the AR-40A's actual output, which has a
±2 dB spec spread.

**As built (no pad):** ~5.04 Vpp EMF into the DA's 75 Ω = **3.02 Vpp**, which is 2×
the DA's 1.5 Vpp maximum. The DA has been running overdriven. `[calc]`

**Target:** ~1.2 Vpp at the DA input. This single point satisfies everything:

| Destination | Level at 1.2 Vpp DA input | Note |
|---|---|---|
| DA input | 1.2 Vpp | inside 0.3–1.5 Vpp spec |
| Orion (75 Ω) | 1.2 Vpp | vs 1 Vpp nominal — 20% over, comparator input, fine |
| Bench instrument (50 Ω) | 0.96 Vpp ≈ +3.6 dBm | 2 × 1.2 × 50/125 |
| New timing board (75 Ω) | 1.2 Vpp | see §6 |

**Pad selection** — measure open-circuit Vpp at the AR-40A SMA with a 10× probe,
then:

| Measured open-circuit | Pad (50 Ω inline, at the rubidium) |
|---|---|
| ~4.0 Vpp (+10 dBm) | 6 dB |
| ~5.0 Vpp (+12 dBm) | 8 dB |
| ~6.3 Vpp (+14 dBm) | 10 dB |

Padding at the source also gives the AR-40A a proper 50 Ω load instead of looking
into 75 Ω, improving source match as a side effect. Buy 3/6/10 dB so combinations
are available.

---

## 5. Grounding topology

Ground paths currently existing between subsystems:

| Path | Notes |
|---|---|
| Extron chassis → 10 MHz coax shields | Extron has a grounded IEC inlet — **mains earth enters the reference chain here** |
| AR-40A T → Si5351 board ground | via coax shield |
| Si5351 board → CM4 | 54 MHz coax shield, grounded both ends — **sole bond** between those islands |
| F9T → CM4 | USB ground + PPS return |
| GNSS antenna coax shield | carries outside potential; termination point unknown `[?]` |
| Supply grounds | topology unconfirmed `[?]` |

The 54 MHz coax on its own is a single clean bond, not a loop. But because the
AR-40A output is T'd, mains earth from the Extron reaches the Si5351 island and
then the CM4 through a chain of shields. Whether current actually flows depends on
whether the CM4 side is separately earthed.

**Consequence if it does:** loop current develops millivolts across the ground that
PPS timestamping is referenced to. This doesn't break anything; it biases and slowly
modulates the threshold crossing, which looks like a small wandering offset rather
than a fault.

**Measurements to settle it:** DMM on AC volts between (a) F9T ground and CM4 ground,
(b) CM4 ground and Extron chassis. More than a few mV means loop current worth
chasing. `[?]`

**Cable thermal note.** PTFE coax (RG-178, RG-316) has a phase-vs-temperature
discontinuity around 19–21 °C, stepping delay by a few hundred ppm over ~2 °C. For a
rig in a room that crosses that point daily this is the largest thermal term in the
distribution. Solid or foam PE behaves better through room temperature. Most
relevant on the GNSS antenna feed, which is the longest run and the only one seeing
outdoor temperature.

---

## 6. Planned board `[plan]`

Replaces the ad-hoc T-and-breakout arrangement on the digital side. JLCPCB, 4-layer.

**Fed from a DA output at ~1.2 Vpp**, therefore:

- **Input:** 75 Ω termination (not 50 Ω — it's downstream of the DA now).
- **Squarer: comparator, mandatory.** At 1.2 Vpp biased at VDD/2 the swing is
  1.05–2.25 V, which clears neither the Si5351's V_IH (2.31 V) nor a 74AHC1GU04's
  thresholds. LTC6957-4 is the purpose-built choice (sine-to-CMOS reference buffer,
  selectable input filtering, sub-ps additive jitter). ADCMP600 or LT1719 also work.
  A CMOS inverter is ruled out at this level.
- **Bias:** 100 nF C0G series, then 10k/10k to VDD/GND with 1 µF on the tap. Use a
  matched pair or array so tempcos track — the ratio sets the threshold.
- **Supply:** dedicated ultra-low-noise LDO (ADP151 / TPS7A20 / LP5907), ferrite +
  10 µF at its input, not shared with anything switching. This matters more than
  temperature: a CMOS threshold tracks VDD directly.
- **Outputs:**
  - squared 10 MHz → Si5351 CLKIN, 33 Ω series, receiver high-Z
  - ÷10⁷ chain → 1 PPS → ZED-F9T EXTINT (4 × 74HC390, or a GreenPAK)
- **Layout:** ground plane under the input, bias network tight to the pin, pour both
  sides and stitch. Controlled impedance unnecessary at 10 MHz over a few cm.

**Why the ÷10⁷ output is the point of the board.** A rubidium-derived 1 PPS on
EXTINT gives continuous UBX-TIM-TM2 time-marks of the AR-40A against GNSS — a
running drift measurement, replacing the oscilloscope phase-drift method in the
AR-40A manual (§3.2.1: 1×10⁻¹⁰ = 100 ns over 1000 s). That closes the measurement
loop even while the disciplining loop stays open.

**Thermal budget check.** At 1.2 Vpp, slew at the zero crossing is ~31 V/µs, so 1 mV
of threshold shift = ~32 ps of phase. Even 1 mV/°C over a 20 °C room swing is ~640 ps
of static offset, a fractional excursion around 10⁻¹³ over hours. The AR-40A's own
temperature spec is ±2×10⁻¹⁰. The conditioning stage is orders of magnitude below
the thing it feeds — it can't affect frequency accuracy, only slowly-varying phase.

**Alternative worth weighing:** feed the board directly from the AR-40A instead of
from a DA output. Gains 2.5 Vpp (~3× better slew, ~3× less threshold-noise jitter)
and removes the board's ground from the Extron's mains earth. Costs a second output
or splitter at the rubidium.

---

## 7. Open items

Ordered by what blocks what.

1. **Measure AR-40A open-circuit output.** Gates the pad choice and everything
   downstream. 10× probe at the SMA.
2. **Fit the pad**, per §4 table.
3. **Verify a DA output is a clean sine** into real loads after padding. Years at
   3 Vpp into a 1.5 Vpp-max input may have left the buffer clipping or slew-limited.
   *This gates the board design* — if the DA output is compromised, the board's input
   level changes and the Extron's role has to be reconsidered. **Don't order until
   this is checked.**
4. **Confirm the Orion's actual AC termination.** BNC T at its input, 10× probe,
   Vpp with the Orion side open vs plugged. R = 50 × V_L / (V_open − V_L).
5. **AC volts between grounds** — F9T↔CM4, CM4↔Extron chassis.
6. **Confirm the F9T breakout's I/O level** is 3.3 V and not shifted to 5 V, before
   anything touches CM4 GPIO.
7. **Decide the F9T data link:** fix USB grounding (cable GND to a header GND pin,
   short; shield terminated at the CM4 end only) or move to UART.
8. **Check what the Si5351 breakout does at its SMA input** — if it already has a
   bias network, the added 50 Ω to ground may have disturbed it.
9. **Resolve the AR-40A BIT polarity contradiction** by observation (§3).
10. **Antenna:** confirm where the coax shield is bonded; if roof-mounted, a
    gas-discharge arrestor at the entry point. A GNSS DC block / galvanic isolator
    removes a whole class of ground problem, provided it passes the active antenna's
    bias.

---

## 8. Things known to be out of spec but working

Kept here so they aren't "fixed" without thought.

- **54 MHz into CM4 XIN at full 3.3 V CMOS swing** through 10 Ω. Crystal inputs
  generally want ~1 Vpp AC-coupled. Stable for years. For PPS timestamping accuracy
  this path is close to irrelevant anyway — a few ps of jitter on the CPU clock
  disappears under interrupt latency. It matters for boot stability and for system
  clock frequency accuracy at long tau, not for edge capture.
- **AR-40A into the Extron at 3.02 Vpp** vs 1.5 Vpp max. Being fixed by the pad.
- **Zero-centred sine into Si5351 CLKIN** (if the 50 Ω shunt was the only thing at
  that node): swings 1.25 V below ground against a −0.5 V absolute maximum, so the
  input protection diode conducts each cycle and acts as the level shifter. Being
  fixed by the new board.
