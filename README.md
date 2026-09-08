# 10 MHz Reference & Timing Rig — Architecture

Status as of 2026-09-07. Written from a rig that was already running; this documents
what exists, what is out of spec, and what is still unmeasured.

**Confidence markers used throughout:**
- `[built]` — how it is actually wired, per direct account
- `[meas]` — measured on this hardware, with date
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
                                 │ (grounded both ends; shares a
                                 │  DC ground with the CM4 via
                                 │  the common RS-15-5 supply)
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

## 2b. Lock interlock — REMOVED 2026-09-07 `[built]`

Previously: the AR-40A's BIT pin (DB9 pin 3) drove a relay coil — coil between +V and
the BIT pin, flyback diode across it — and the relay switched 5 V to the CM4, Teensy
and Si5351 board, with an LED indicating the same state. Ran for years. **Removed;
all three now power up directly.**

Note the AR-40A's 10 MHz output is present from power-up; the specified 5 minutes is
time to *lock*, not time to output. So the interlock was never required for the CM4
to boot.

Two consequences of removing it:

- **Warm-up frequency step.** The Si5351 now locks PLLA to the free-running OCXO
  (~10⁻⁷ off) at power-up, the CM4 boots on that, and the frequency shifts as the
  physics package pulls in ~5 min later. chrony copes but re-converges its frequency
  estimate after every boot.
- **No clean failure on a dead rubidium.** With no CLKIN, PLLA never locks and CLK4
  output is undefined rather than absent. The relay made this a clean "stays off".

**Replacement, decided 2026-09-08 `[plan]`:** BIT goes to **Teensy GPIO pin 10**,
`INPUT_PULLUP`, LOW = locked. The Teensy holds CLK4 disabled until lock, then enables
it, and reports lock state to the CM4. Nothing in the 5 V path.

- BIT is open-collector, so with the internal pull-up no external voltage reaches the
  pin. (Teensy 3.2 digital pins are 5 V tolerant anyway — it's the 3.5/3.6 and 4.x
  that aren't.)
- 1 kΩ in series at the pin for ESD, 100 nF pin-to-ground to keep RF off it.
- Pin 10 is also SPI0 CS on a Teensy 3.2 — leave it free if SPI is ever wanted.

**Optocoupler: not needed — earlier advice in this document was wrong.** The rule
about second inter-domain paths is about *low-impedance* ones: a shield bond is
milliohms and carries real current. This path sits behind a pull-up of tens of kΩ, so
it can carry only microamps, and the 10 MHz coax shield already bonds those two
grounds at low impedance regardless. A high-impedance signal path alongside an
existing low-impedance bond changes nothing.

The one case that would change it: if AR-40A ground and Teensy ground differed by
more than a few hundred mV, the saturated BIT transistor would pull the pin below the
Teensy's ground and forward-bias its ESD diode. The shield ties them, so expect
millivolts — confirmed by the AC-volts check in §7. `[?]`

---

## 3. Component inventory

### AccuBeat AR-40A rubidium frequency standard
| Parameter | Value | Src |
|---|---|---|
| Output | 10 MHz sine, +12±2 dBm into 50 Ω | `[spec]` |
| **Measured open-circuit** | **4.44 Vpp, 1.55 V rms** (2026-09-07) | `[meas]` |
| → implied into 50 Ω | 2.22 Vpp = +10.9 dBm | `[calc]` |
| → implied into 75 Ω | 2.66 Vpp | `[calc]` |
| **Measured supply current** | **1.3 A warm-up, 0.600 A steady @ 15 Vdc; settled in <5 min** | `[meas]` |
| Aging | <1×10⁻⁹ first year, <5×10⁻¹⁰/yr after | `[spec]` |
| Allan deviation | <3×10⁻¹¹ @ 1 s, <3×10⁻¹² @ 100 s | `[spec]` |
| Stability vs temp | ±2×10⁻¹⁰ over −5…+50 °C | `[spec]` |
| Accuracy at shipment | 5×10⁻¹¹ | `[spec]` |
| Supply | 15 Vdc ±5%, ~0.6 A steady, ~1.7 A warm-up | `[spec]` |
| Warm-up | 5 min to lock, 7.5 min to 5×10⁻¹⁰ | `[spec]` |
| Mech. trim | trimmer under calibration sticker, 1 turn ≈ 5×10⁻¹⁰, 10 turns | `[spec]` |
| BIT | DB9 pin 3, open-collector | `[spec]` |

**Source impedance measured, not assumed.** The manual states delivered power into a
50 Ω load; it does not state that the output *is* a 50 Ω source. It was measured at
46.2 Ω — see below — so the figures above hold.

**Source impedance: measured 46.2 Ω** `[meas]` — 4.44 Vpp open, 2.24 Vpp across a
measured 47 Ω terminator (10 cm coax), Zs = R × (V_open − V_L)/V_L. A 50 Ω source
within measurement error. Available power +11.25 dBm, inside the +12±2 dBm spec.
Crest factor consistent open and loaded (2.87 / 2.90 vs 2.828 ideal) — the ~2.5%
excess is systematic peak-detect noise, not load-dependent distortion.

DMM DC resistance at the SMA reads ~84 Ω. `[meas]` This does **not** establish the RF
source impedance — DC resistance and impedance at 10 MHz are different quantities.
It does rule out a transformer- or capacitor-coupled output (either would read open),
so there is a DC-conductive resistive network to the centre pin. Superseded by the
AC measurement above; kept only to record that the two disagree.

**Manual inconsistency — RESOLVED.** The AR-40A manual states BIT polarity two ways
that contradict each other:

- §1.3 spec table and §2.1.2: locked = pin 3 shorted to ground ("0")
- §3.3: "0" logic described as *open collector* = lock; "1" = *short to ground* = unlock

**§2.1.2 is correct; §3.3 has its labels swapped.** `[built]` Resolved empirically by
the existing interlock circuit: relay coil between +V and the BIT pin, flyback diode
across the coil, and the CM4 powers up *after* lock. The pin therefore sinks coil
current when locked. This has run for years.

Constraint worth recording: AccuBeat do not specify the BIT open collector's sink
current. It is adequate for the present relay coil, but it is an undocumented rating
— if the relay is ever replaced with something drawing more, drive a logic-level FET
from the pin instead.

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

**Measured 2026-09-07** `[meas]`: Gain/Peak DIP **off**, AC coupling **on**, verified.
R output unloaded (1 MΩ, coax direct, no probe): **2.52 Vpp, 892 mV rms**, against
2.49 Vpp predicted for unity gain into no termination. Crest factor 2.825 vs 2.828
ideal — no peak flattening, no slew limiting. Buffer healthy unloaded.

This also revises an earlier note: the ~2% crest-factor excess on all the probe
measurements was the 10× probe, not a scope systematic. Coax direct is the better
measurement here.

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

The breakout has its own onboard regulator `[built]`, so the Si5351's VDD — and
therefore its CLKIN thresholds — is already decoupled from CM4 transients on the
shared 5 V rail. Part number unidentified, so PSRR above ~10 kHz is unknown. `[?]`
This does not remove the need for a dedicated LDO on the new board (the comparator's
threshold tracks *its* supply), nor the star-grounding point in §5 — a regulator
holds VDD steady against its own ground pin, and does nothing about ground currents
moving that reference relative to the incoming coax shield.

### Raspberry Pi CM4
- BCM2711 crystal removed; 54 MHz injected at XIN from Si5351 CLK4. `[built]`
- Out of spec on paper (crystal inputs generally want ~1 Vpp AC-coupled; a 10 Ω
  series resistor into a high-Z XIN passes essentially the full 3.3 V CMOS swing),
  but stable in service for years. `[built]`
- Dedicated realtime Linux. `[built]`
- chrony, stratum-1 NTP/PTP source. `[built]`
- Powered from a MeanWell RS-15-5 shared with the Teensy and Si5351 board. `[built]`
  15 W / 3 A at 5 V — check headroom, the CM4 alone can take over half of it under
  load. `[?]`
- **Carrier: standard Raspberry Pi CM4 IO Board.** 5 V fed into J20 (4-pin Berg).
  Pin assignment verified by continuity on this board: **J20 pin 3 = GND, pin 4 =
  +5 V.** Nothing on J19. `[meas]`
- **L5 removed** `[built]` — the documented modification for feeding external 5 V. It
  stops the onboard 5 V and 3.3 V supplies starting up and keeps 5 V off the DC jack.
  Consequence: no 12 V rail, so the PCIe slot can't power a card needing it and the
  fan header may be dead. Don't reinstate L5 without reconsidering the supply.
- Note the 40-pin header's 5 V pins are the same net as J20's — feeding either
  backfeeds the onboard supply identically. The header is not a way around L5.

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

**As built (no pad):** 4.44 Vpp EMF from a 46.2 Ω source into the DA's 75 Ω =
**2.75 Vpp**, i.e. 1.83× the DA's 1.5 Vpp maximum. The DA has been running
overdriven. `[calc]`

**Pad: built and verified.** `[meas]` DIY 50 Ω pi, measured values 150.000 Ω /
50.975 Ω / 149.337 Ω = **7.01 dB**. Mounted between the AR-40A SMA and an output BNC.
Verification 2026-09-07: 992 mV–1.0 Vpp across a measured 47 Ω terminator, against
998 mV predicted — within 1%. Crest factor 2.89, matching the 2.87 / 2.90 measured
open and loaded before the pad, so the ~2% excess over 2.828 is instrument
systematic, not distortion.

Resulting levels:

| Destination | Level | Note |
|---|---|---|
| DA input | 1.245 Vpp | 17% margin under the 1.5 Vpp max |
| Orion (75 Ω) | 1.245 Vpp | 25% over 1 Vpp nominal; comparator input |
| Bench instrument (50 Ω) | 0.996 Vpp ≈ +3.9 dBm | 2 × V × 50/125 |
| New timing board (75 Ω) | 1.245 Vpp | see §6 |
| Impedance seen by AR-40A | 60.4 Ω, ~20 dB return loss | vs 75 Ω unpadded |

Keep the DA gain DIP at unity, not +1.1 dB.

**Pad enclosure grounding:** irrelevant either way. The AR-40A's output ground is its
chassis ground, which is earthed, so the pad sits inside an already-earthed path —
isolating its connectors from its own box changes nothing (§5).

**Supply.** Measured 1.3 A @ 15 Vdc warm-up, settling sharply to **0.600 A in under
5 minutes** — matches the 0.6 A steady-state spec and the 5 min warm-up figure
exactly. Oven reaching setpoint promptly; no indication of an aging lamp or a
struggling thermal loop. Unit is healthy. `[meas]`

19.5 W warm-up / 9 W steady, so size the supply for 2 A and respect the manual's
≤1 °C/W heatsink recommendation — 9 W in a small box feeds straight into the
±2×10⁻¹⁰ temperature spec.

---

## 5. Grounding topology

Ground paths currently existing between subsystems:

| Path | Notes |
|---|---|
| AR-40A module case → enclosure chassis | bonded `[built]` |
| AR-40A SMA output ground → chassis | bonded — **the reference chain's signal ground is the AR-40A chassis** `[built]` |
| AR-40A PSU ground → chassis | bonded `[built]` |
| AR-40A chassis → mains PE | **unknown — depends on the supply's inlet** `[?]` |
| Extron chassis → 10 MHz coax shields | Extron has a grounded IEC inlet — **mains earth enters the reference chain here** |
| AR-40A T → Si5351 board ground | via coax shield |
| Si5351 board → CM4 | 54 MHz coax shield, grounded both ends |
| Teensy + Si5351 + CM4 supply | **all three on one MeanWell RS-15-5 (5 V)** — they share a DC ground through the supply wiring `[built]` |
| AR-40A supply | MeanWell LRS-75-15 (15 V), separate — **−V deliberately not tied to the 5 V supply's −V** `[built]` |
| F9T → CM4 | USB ground + PPS return |
| GNSS antenna coax shield | carries outside potential; termination point unknown `[?]` |
| Supply grounds | topology unconfirmed `[?]` |

**Correction to the "floating island" framing.** The digital island is *not* floating
once the 10 MHz coax is connected. The shield must bond to the Si5351 board's ground
to serve as the signal return, and that shield runs back to the earthed Extron. So
the island is earth-referenced through the coax shield, unavoidably. Isolating the
BNC from its enclosure prevents a *second* path via chassis and rack; it cannot
isolate the shield from board ground.

The goal is therefore **one path between domains**, not a floating island. That path
is the coax shield.

Follows from that: **do not tie the LRS-75-15's −V (AR-40A supply) to the RS-15-5's
−V.** The 15 V supply's −V sits at the AR-40A chassis, which is earthed; the digital
island reaches the same earth via the Extron and the coax shield. Bonding them adds
a second path and closes a bench-sized loop. The two domains exchange no signal
needing a shared DC reference — only the 10 MHz coax, which carries its own return,
and the interlock relay contacts, which are galvanically isolated. `[built]`

**Correction (2026-09-07).** An earlier draft of this document treated the
Teensy/Si5351 board and the CM4 as separate ground islands bonded only by the 54 MHz
coax shield. That is wrong: all three run from one RS-15-5, so they share a DC ground
through the supply wiring. The coax shield plus the supply return therefore form a
real loop, of area determined by cable routing.

Consequences and mitigations:

- **Route the 54 MHz coax alongside the DC supply pair.** Same connections, minimal
  enclosed area, no cost.
- **Star the DC distribution from the PSU terminals**, separate pairs per board, not
  daisy-chained. This matters more than the loop: the CM4 draws >1 A with fast load
  transients while the Si5351 draws tens of mA. A shared return conductor puts CM4
  CPU activity onto the ground that the Si5351's CLKIN threshold is referenced to.
  Exception: the Teensy chains off the Si5351 board, which is correct — they are
  physically attached, the Teensy's draw is small and near-constant, and a separate
  run back to the PSU would enclose a larger loop between their grounds than it
  removes.
- **Cable:** audio multicore inner pairs work — hot to +5 V, cold to return (already
  twisted, so minimal loop area), shield bonded to −V at the PSU end only and left
  open at the board. Do not use the shield as the current return. Watch the CM4 leg:
  24–26 AWG gives 84–134 mΩ round trip over 0.5 m, i.e. 126–201 mV drop at 1.5 A —
  parallel several pairs or use heavier wire, and verify ≥4.9 V at the CM4 under
  load. `[?]`
- **RS-15-5 earth behaviour: confirmed isolated.** `[meas]` Infinite resistance
  measured between the FG terminal and the DC output, so the output floats
  galvanically. Its only coupling to earth is the Y-capacitor (a few ohms at 10 MHz,
  megohms at mains frequency).
- **DC distribution as built `[built]`:** three separate 5 V runs from the PSU
  terminals — one to the CM4, one to the Si5351/Teensy pair, one to the ZED-F9T.
  Note the F9T's separate run keeps CM4 load current off its reference but does not
  make it a separate island: its ground is still tied to the CM4's through the USB
  and PPS returns, and should be.
- **New BNC input on the Si5351 enclosure (fed from DA R out 2): isolate it.** The
  Y-cap RF bond can't be removed; the DC/mains-frequency loop that a bonded connector
  would add can be. Shield to the board's ground pour only.

**Two possible earth injections.** If the AR-40A chassis reaches mains PE, the
reference chain is earthed at both ends — AR-40A and Extron — and the 10 MHz coax
closes the loop. Test: unplug the AR-40A supply, DMM continuity from enclosure to
the earth pin of its mains inlet. `[?]`

**This does not change the pad.** Because the AR-40A's output ground is already its
chassis ground, isolating connectors on a pad hanging off that output achieves
nothing — the shield arrives bonded. Bond the pad enclosure and move on. Isolated
bulkhead connectors are only relevant for boxes that introduce a *new* earth
reference, e.g. a rack-mounted timing board with its own earthed supply.

**Priority judgement.** Loop current on the 10 MHz shield puts millivolts on a
1.245 Vpp signal — 60–70 dB down — into comparator inputs. That is a small periodic
phase modulation at mains frequency, a few ps at 42 V/µs slew, averaging out over any
useful measurement interval. It does not touch frequency accuracy. Ground noise on
the **PPS path** (F9T↔CM4) is the one that matters, because it shifts a single edge
that nothing averages. Protect that island; don't spend effort breaking the 10 MHz
loop until measurements justify it. Never lift a safety earth for either.

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

**Fed from a DA output at 1.245 Vpp** (7.01 dB pad, §4), therefore:

- **Input:** 75 Ω termination (not 50 Ω — it's downstream of the DA now).
- **Squarer: comparator, mandatory.** At 1.245 Vpp biased at VDD/2 the swing is
  1.03–2.27 V, inside the Si5351's V_IL (0.99 V) and V_IH (2.31 V) — it does not
  reach either threshold. A 74AHC1GU04 is no better placed. LTC6957-4 is the
  purpose-built choice (sine-to-CMOS reference buffer, selectable input filtering,
  sub-ps additive jitter). ADCMP600 or LT1719 also work. A CMOS inverter is ruled
  out at this level.
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

**Thermal budget check.** At 1.245 Vpp, slew at the zero crossing is ~39 V/µs, so
1 mV of threshold shift = ~26 ps of phase. Even 1 mV/°C over a 20 °C room swing is
~510 ps of static offset, a fractional excursion around 10⁻¹³ over hours. The
AR-40A's own temperature spec is ±2×10⁻¹⁰. The conditioning stage is orders of
magnitude below the thing it feeds — it can't affect frequency accuracy, only
slowly-varying phase.

**Alternative worth weighing:** feed the board directly from the AR-40A instead of
from a DA output. Gains 2.22 Vpp (~1.7× better slew, proportionally less
threshold-noise jitter) and removes the board's ground from the Extron's mains earth.
Costs a second output or splitter at the rubidium.

---

## 7. Open items

Ordered by what blocks what.

1. ~~**Measure AR-40A open-circuit output and source impedance.**~~ **Done
   2026-09-07: 4.44 Vpp open, 46.2 Ω source, +11.25 dBm available. Closed.**
2. ~~**Fit the pad.**~~ **Done 2026-09-07: DIY 7.01 dB pi, verified to within 1% of
   prediction. Closed.**
3. **Verify a DA output is a clean sine** into real loads after padding. Years at
   3 Vpp into a 1.5 Vpp-max input may have left the buffer clipping or slew-limited.
   *This gates the board design* — if the DA output is compromised, the board's input
   level changes and the Extron's role has to be reconsidered. **Don't order until
   this is checked.**
4. **Confirm the Orion's actual AC termination.** BNC T at its input, 10× probe,
   Vpp with the Orion side open vs plugged. R = 50 × V_L / (V_open − V_L).
5. **AC volts between grounds** — F9T↔CM4, CM4↔Extron chassis.
5b. **Is the AR-40A chassis at mains PE?** Unplugged continuity check, enclosure to
    inlet earth pin. Determines whether the 10 MHz chain has one earth injection or
    two (§5).
6. **Confirm the F9T breakout's I/O level** is 3.3 V and not shifted to 5 V, before
   anything touches CM4 GPIO.
7. **Decide the F9T data link:** fix USB grounding (cable GND to a header GND pin,
   short; shield terminated at the CM4 end only) or move to UART.
8. **Check what the Si5351 breakout does at its SMA input** — if it already has a
   bias network, the added 50 Ω to ground may have disturbed it.
9. ~~**Resolve the AR-40A BIT polarity contradiction.**~~ **Closed** — settled by the
   existing lock interlock, see §3.
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
