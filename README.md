# 10 MHz Reference & Timing Rig — Architecture

Status 2026-09-09. Rig already running. Doc say what exist, what out of spec, what
not measured yet.

**Confidence marks:**
- `[built]` — how wired, direct account
- `[meas]` — measured on this hardware, with date
- `[spec]` — datasheet/manual, not verified here
- `[calc]` — derived from `[spec]` numbers
- `[?]` — unverified, need measurement
- `[plan]` — not built yet

---

## 1. Purpose

Two goals, one rubidium:

1. **Frequency reference.** Short-term stability + holdover for audio clock
   (Orion 32 HD) and bench gear.
2. **Time reference.** UTC-traceable absolute time on CM4. CM4 run chrony as
   stratum-1 NTP/PTP source.

Complementary, not redundant: AR-40A give stability, ZED-F9T give traceability.
GPSDO architecture split across boxes. Disciplining loop open now (see §7).

**Not** goal: Dante or AES67 grandmaster.

---

## 2. Block diagram (current)

```
                    AccuBeat AR-40A
              10 MHz sine, 4.44 Vpp open, 46.2 Ω
                           │
                           │  SMA
                           ▼
                  ┌──────────────────┐
                  │ 7.01 dB pi pad   │  150 / 50.975 / 149.337 Ω
                  │ at the rubidium  │  (DIY, verified)
                  └────────┬─────────┘
                           │  1.245 Vpp
                           ▼
                  ┌──────────────────┐
                  │ Extron DA        │  75 Ω in, unity gain,
                  │ RGB/YUV, "R"     │  Gain/Peak off, AC coupled
                  └──┬────┬───────┬──┘
                     │    │       │   isolated outputs, 75 Ω build-out
        ┌────────────┘    │       └────────────┐
        ▼                 ▼                    ▼
   Orion 32 HD      bench instruments    Si5351C breakout
   10M input, sine  (50 Ω, 0.996 Vpp)    CLKIN, unterminated
   1.245 Vpp        `[plan]`             2.49 Vpp nominal,
   `[plan]`                              2.1 Vpp measured
                                         (clamped, see §8)
                                              │
              Teensy 3.2 ── I2C (18/19) ──────┤
              (BIT lock → pin 2)              │
                                              ▼
                                        CLK4, 8 mA
                                        54 MHz
                                              │ coax + 10 Ω series
                                              │ damping, grounded
                                              │ both ends
                                              ▼
                        ┌────────────────┐
                        │  RPi CM4       │  BCM2711 XIN
                        │  realtime Linux│  (crystal removed)
                        │  chrony        │  CM4 IO carrier, L5 out,
                        └────────────────┘  5 V into J20 pin 4 / GND pin 3
                             ▲       ▲
                     1 PPS ──┘       └── USB (D+/D- on
                  (PTP hw irq)            breakout header)
                             │              │
                        ┌────┴──────────────┴───┐
                        │  u-blox ZED-F9T       │
                        │  breakout             │
                        └───────────────────────┘
                              │            │ SMA
                          EXTINT           ▼
                        (from new     GNSS antenna
                         board) `[plan]`
```

Power: one MeanWell RS-15-5 (5 V), three separate runs from its terminals — CM4,
Si5351/Teensy, ZED-F9T. AR-40A on own LRS-75-15 (15 V), −V deliberately not tied to
5 V supply. See §5.

**Superseded arrangement**, kept so nobody reintroduce it: AR-40A output split by
passive soldered T, unpadded, feeding Extron and Si5351 CLKIN direct. Put 2.75 Vpp
into 1.5 Vpp-max input, gave two destinations no isolation from each other. Replaced
by pad + one DA output per destination.

---

## 2b. Lock interlock — REMOVED 2026-09-07 `[built]`

Before: AR-40A BIT pin (DB9 pin 3) drove relay coil — coil between +V and BIT pin,
flyback diode across it — relay switched 5 V to CM4, Teensy, Si5351 board, LED showed
same state. Ran for years. **Removed; all three now power up direct.**

Note: AR-40A 10 MHz output present from power-up. Specified 5 minutes is time to
*lock*, not time to output. So interlock never required for CM4 to boot.

Two consequences of removal:

- **Warm-up frequency step.** Si5351 now lock PLLA to free-running OCXO (~10⁻⁷ off)
  at power-up, CM4 boot on that, frequency shift as physics package pull in ~5 min
  later. chrony cope but re-converge frequency estimate after every boot.
- **No clean failure on dead rubidium — MEASURED 2026-09-09.** `[meas]` With 5 V
  applied, 10 MHz BNC open, CM4 unpowered: CLK4 make periodic **~11.6 MHz** output
  (counter reading; confirmed by eye at 20 ns/div), 1.72 Vpp / 686 mV rms into CM4
  clamps. PLL loop open, charge pump rail, VCO park ~186 MHz — MS4 ÷16 give 11.6 MHz.
  600–900 MHz VCO figure is specified range *with* reference, not physical bound.

  Worst of three possible outcomes: not silence, not obviously broken, but
  stable-looking clock at ~fifth of nominal. CM4 internal PLLs configured around
  54 MHz, likely not lock at all, so symptom = board with power that do nothing. Hard
  to diagnose cold.

  Amplitude and frequency both contaminated by unpowered CM4 input clamps conducting
  — with CM4 powered, frequency unchanged at 11.6 MHz, amplitude rise to 2.36 Vpp /
  ~896 mV rms as clamps stop conducting. `[meas]` (Still below full 3.3 V CMOS swing
  because scope 100 MHz bandwidth round the edges; crest factor 2.63 sit between
  square and sine — signature of that filtering.)

  **CM4 symptom with invalid XIN clock, confirmed `[meas]`:** power + green LEDs lit,
  static, no activity flicker. SoC internal PLLs never lock at 11.6 MHz so boot ROM
  never run. Field diagnostic — two LEDs lit, no activity = no valid clock at XIN.

**This make Teensy lock-gating necessary, not optional.** Hold CLK4 disabled until BIT
read locked → case become clean no-clock. Drive LED from BIT state same time → get
positive "reference not locked" indication. Same info old relay LED gave, without
relay, better than inferring from Pi that won't boot.

**Replacement, decided 2026-09-08 `[plan]`:** BIT → **Teensy GPIO pin 10**,
`INPUT_PULLUP`, LOW = locked. Teensy hold CLK4 disabled until lock, then enable it,
and report lock state to CM4. Nothing in 5 V path.

- BIT is open-collector, so with internal pull-up no external voltage reach pin.
  (Teensy 3.2 digital pins 5 V tolerant anyway — 3.5/3.6 and 4.x are not.)
- 1 kΩ series at pin for ESD, 100 nF pin-to-ground to keep RF off it.
- Pin 10 also SPI0 CS on Teensy 3.2 — leave free if SPI ever wanted.

**Optocoupler: not needed — earlier advice in this doc was wrong.** Rule about second
inter-domain paths is about *low-impedance* ones: shield bond is milliohms, carry real
current. This path sit behind pull-up of tens of kΩ, so carry only microamps, and
10 MHz coax shield already bond those two grounds at low impedance regardless.
High-impedance signal path alongside existing low-impedance bond change nothing.

One case that would change it: if AR-40A ground and Teensy ground differ by more than
few hundred mV, saturated BIT transistor pull pin below Teensy ground and forward-bias
its ESD diode. Shield tie them, so expect millivolts — confirm with AC-volts check in
§7. `[?]`

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

**Source impedance measured, not assumed.** Manual state delivered power into 50 Ω
load; it not state output *is* 50 Ω source. Measured 46.2 Ω — see below — so figures
above hold.

**Source impedance: measured 46.2 Ω** `[meas]` — 4.44 Vpp open, 2.24 Vpp across
measured 47 Ω terminator (10 cm coax), Zs = R × (V_open − V_L)/V_L. 50 Ω source within
measurement error. Available power +11.25 dBm, inside +12±2 dBm spec. Crest factor
consistent open and loaded (2.87 / 2.90 vs 2.828 ideal) — ~2.5% excess is systematic
peak-detect noise, not load-dependent distortion.

DMM DC resistance at SMA read ~84 Ω. `[meas]` This **not** establish RF source
impedance — DC resistance and impedance at 10 MHz are different quantities. It do rule
out transformer- or capacitor-coupled output (either would read open), so DC-conductive
resistive network to centre pin exist. Superseded by AC measurement above; kept only to
record that the two disagree.

**Manual inconsistency — RESOLVED.** AR-40A manual state BIT polarity two contradictory
ways:

- §1.3 spec table and §2.1.2: locked = pin 3 shorted to ground ("0")
- §3.3: "0" logic described as *open collector* = lock; "1" = *short to ground* = unlock

**§2.1.2 correct; §3.3 have labels swapped.** `[built]` Resolved empirically by existing
interlock circuit: relay coil between +V and BIT pin, flyback diode across coil, CM4
power up *after* lock. Pin therefore sink coil current when locked. Ran for years.

Constraint worth recording: AccuBeat not specify BIT open collector sink current.
Adequate for present relay coil, but undocumented rating — if relay ever replaced with
something drawing more, drive logic-level FET from pin instead.

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

Also revise earlier note: ~2% crest-factor excess on all probe measurements was the 10×
probe, not scope systematic. Coax direct is better measurement here.

**Usage constraints:**
- Use **R, G or B** planes (or Y on YUV A). H/V sync inputs are 510 Ω into TTL squarer
  spec'd 15–180 kHz — useless at 10 MHz.
- On DA6 YUV A, avoid digital audio BNC: 510 Ω input, 2.5 Vpp output.
- DIP switches: Gain/Peak **off** (no shaping on reference sine). AC coupling safer
  default.
- R, G, B are independent signal paths sharing one chassis, so 6-output unit fed on R
  leave G and B as two more idle 1→6 buses.

### Antelope Orion 32 HD — 10M input
| Parameter | Value | Src |
|---|---|---|
| Signal | sine | `[built]` |
| Impedance | 75 Ω | `[spec]` |
| Level | 1 Vpp nominal | `[spec]` |
| Front end | comparator / squarer, self-biasing | `[spec]` |
| Actual AC termination | unconfirmed | `[?]` |

Note: every Orion clock mode discipline same internal OCXO. Advantage of 10M over word
clock is multiplication ratio (~×2.26 vs ×512), i.e. far less phase-noise amplification.
10M is right input to use.

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

**Do not** feed 10 MHz into XA — that input specified 25–27 MHz only.

Breakout have own onboard regulator `[built]`, so Si5351 VDD — and therefore CLKIN
thresholds — already decoupled from CM4 transients on shared 5 V rail. Part number
unidentified, so PSRR above ~10 kHz unknown. `[?]` This not remove need for dedicated
LDO on new board (comparator threshold track *its* supply), nor star-grounding point in
§5 — regulator hold VDD steady against its own ground pin, do nothing about ground
currents moving that reference relative to incoming coax shield.

### Raspberry Pi CM4
- BCM2711 crystal removed; 54 MHz injected at XIN from Si5351 CLK4. `[built]`
- Out of spec on paper (crystal inputs generally want ~1 Vpp AC-coupled; 10 Ω series
  into high-Z XIN pass essentially full 3.3 V CMOS swing), but stable in service for
  years. `[built]`
- Dedicated realtime Linux. `[built]`
- chrony, stratum-1 NTP/PTP source. `[built]`
- Powered from MeanWell RS-15-5 shared with Teensy and Si5351 board. `[built]`
  15 W / 3 A at 5 V — check headroom, CM4 alone can take over half under load. `[?]`
- **Carrier: standard Raspberry Pi CM4 IO Board.** 5 V fed into J20 (4-pin Berg). Pin
  assignment verified by continuity on this board: **J20 pin 3 = GND, pin 4 = +5 V.**
  Nothing on J19. `[meas]`
- **L5 removed** `[built]` — documented mod for feeding external 5 V. Stop onboard 5 V
  and 3.3 V supplies starting up, keep 5 V off DC jack. Consequence: no 12 V rail, so
  PCIe slot can't power card needing it and fan header may be dead. Don't reinstate L5
  without reconsidering supply.
- Note 40-pin header 5 V pins are same net as J20 — feeding either backfeed onboard
  supply identically. Header not a way around L5.

### u-blox ZED-F9T on breakout
- I/O 3.3 V at module. Whether *this breakout* level-shift to 5 V unconfirmed. `[?]`
- Header: +5V, GND, RX(2)/TX(2), TIME(1)/TIME(2), EXTINT, READY, SCL/SPI_CLK,
  SDA/SPI_CS, USB D−/D+, RX/SPI_MOSI, TX/SPI_MISO. SEL pad select UART+I²C vs SPI.
- TIME(1) = TIMEPULSE1 = 1 PPS default → CM4 PTP hardware interrupt. `[built]`
- Data to CM4 over USB, ordinary USB cable on D+/D− header pins. Worked, but grounding
  sketchy in several places. `[built]`
- EXTINT unused. Available for UBX-TIM-TM2 time-marking. `[plan]`

---

## 4. Levels

Everything downstream set by one number: AR-40A actual output, ±2 dB spec spread.

**As built (no pad):** 4.44 Vpp EMF from 46.2 Ω source into DA 75 Ω = **2.75 Vpp**,
1.83× DA 1.5 Vpp maximum. DA been running overdriven. `[calc]`

**Pad: built and verified.** `[meas]` DIY 50 Ω pi, measured values 150.000 Ω /
50.975 Ω / 149.337 Ω = **7.01 dB**. Mounted between AR-40A SMA and output BNC.
Verification 2026-09-07: 992 mV–1.0 Vpp across measured 47 Ω terminator, against 998 mV
predicted — within 1%. Crest factor 2.89, match 2.87 / 2.90 measured open and loaded
before pad, so ~2% excess over 2.828 is instrument systematic, not distortion.

Rebuilt 2026-09-09 (series and shunt parts replaced). DC port resistance, far port open:
**85.644 Ω input, 85.565 Ω output** `[meas]`, against 85.75 Ω predicted for
150 ∥ (51 + 150). Network intact, symmetric to 0.09%. Not same quantity as 55.7 Ω
terminated input impedance — open-port DC resistance and terminated input impedance are
different measurements of same network. Also: open-port resistance not uniquely
determine attenuation (120/179/120 read similar and is much bigger pad), so re-run
functional check when AR-40A supply return: 4.44 Vpp in, expect ~1.0 Vpp across 47 Ω
terminator. `[?]`

Resulting levels:

| Destination | Level | Note |
|---|---|---|
| DA input | 1.245 Vpp | 17% margin under 1.5 Vpp max |
| Orion (75 Ω) | 1.245 Vpp | 25% over 1 Vpp nominal; comparator input |
| Bench instrument (50 Ω) | 0.996 Vpp ≈ +3.9 dBm | 2 × V × 50/125 |
| New timing board (75 Ω) | 1.245 Vpp | see §6 |
| Impedance seen by AR-40A | 60.4 Ω, ~20 dB return loss | vs 75 Ω unpadded |

Keep DA gain DIP at unity, not +1.1 dB.

**Pad enclosure grounding:** irrelevant either way. AR-40A output ground is its chassis
ground, which is earthed, so pad sit inside already-earthed path — isolating its
connectors from its own box change nothing (§5).

**Supply.** Measured 1.3 A @ 15 Vdc warm-up, settling sharply to **0.600 A in under
5 minutes** — match 0.6 A steady-state spec and 5 min warm-up figure exactly. Oven reach
setpoint promptly; no sign of aging lamp or struggling thermal loop. Unit healthy.
`[meas]`

19.5 W warm-up / 9 W steady, so size supply for 2 A and respect manual ≤1 °C/W heatsink
recommendation — 9 W in small box feed straight into ±2×10⁻¹⁰ temperature spec.

---

## 5. Grounding topology

Ground paths currently existing between subsystems:

| Path | Notes |
|---|---|
| AR-40A module case → enclosure chassis | bonded `[built]` |
| AR-40A SMA output ground → chassis | bonded — **reference chain signal ground is AR-40A chassis** `[built]` |
| AR-40A PSU ground → chassis | bonded `[built]` |
| AR-40A chassis → mains PE | **unknown — depend on supply inlet** `[?]` |
| Extron chassis → 10 MHz coax shields | Extron have grounded IEC inlet — **mains earth enter reference chain here** |
| AR-40A T → Si5351 board ground | via coax shield |
| Si5351 board → CM4 | 54 MHz coax shield, grounded both ends |
| Teensy + Si5351 + CM4 supply | **all three on one MeanWell RS-15-5 (5 V)** — share DC ground through supply wiring `[built]` |
| AR-40A supply | MeanWell LRS-75-15 (15 V), separate — **−V deliberately not tied to 5 V supply −V** `[built]` |
| F9T → CM4 | USB ground + PPS return |
| GNSS antenna coax shield | carry outside potential; termination point unknown `[?]` |
| Supply grounds | topology unconfirmed `[?]` |

**Correction to "floating island" framing.** Digital island *not* floating once 10 MHz
coax connected. Shield must bond to Si5351 board ground to serve as signal return, and
that shield run back to earthed Extron. So island is earth-referenced through coax
shield, unavoidably. Isolating BNC from enclosure prevent *second* path via chassis and
rack; cannot isolate shield from board ground.

Goal therefore **one path between domains**, not floating island. That path is coax
shield.

Follows: **do not tie LRS-75-15 −V (AR-40A supply) to RS-15-5 −V.** 15 V supply −V sit
at AR-40A chassis, which is earthed; digital island reach same earth via Extron and coax
shield. Bonding them add second path, close bench-sized loop. Two domains exchange no
signal needing shared DC reference — only 10 MHz coax, which carry own return, and
interlock relay contacts, galvanically isolated. `[built]`

**Correction (2026-09-07).** Earlier draft treated Teensy/Si5351 board and CM4 as
separate ground islands bonded only by 54 MHz coax shield. Wrong: all three run from one
RS-15-5, so they share DC ground through supply wiring. Coax shield + supply return
therefore form real loop, area set by cable routing.

Consequences and mitigations:

- **Route 54 MHz coax alongside DC supply pair.** Same connections, minimal enclosed
  area, no cost.
- **Star DC distribution from PSU terminals**, separate pairs per board, not
  daisy-chained. Matter more than the loop: CM4 draw >1 A with fast load transients
  while Si5351 draw tens of mA. Shared return conductor put CM4 CPU activity onto ground
  that Si5351 CLKIN threshold is referenced to. Exception: Teensy chain off Si5351 board
  — correct, they physically attached, Teensy draw small and near-constant, separate run
  back to PSU would enclose bigger loop between their grounds than it remove.
- **Rails measured at far end of each cable 2026-09-09** `[meas]`: CM4 4.936 V idle /
  **4.917 V booted under load**, Teensy 4.932 V, Si5351 4.937 V, F9T 4.955 V. 23 mV
  spread across four, ~60 mV total drop from nominal — flat readings confirm star
  topology. All comfortably above 4.75 V threshold; no leg need paralleling.
- **Cable:** audio multicore inner pairs work — hot to +5 V, cold to return (already
  twisted, minimal loop area), shield bonded to −V at PSU end only, open at board. Do
  not use shield as current return. Watch CM4 leg: 24–26 AWG give 84–134 mΩ round trip
  over 0.5 m = 126–201 mV drop at 1.5 A. Measured as built: 4.917 V at CM4 under load —
  adequate, no action needed. `[meas]`
- **RS-15-5 earth behaviour: confirmed isolated.** `[meas]` Infinite resistance between
  FG terminal and DC output, so output float galvanically. Only coupling to earth is
  Y-capacitor (few ohms at 10 MHz, megohms at mains frequency).
- **DC distribution as built `[built]`:** three separate 5 V runs from PSU terminals —
  one to CM4, one to Si5351/Teensy pair, one to ZED-F9T. F9T separate run keep CM4 load
  current off its reference but not make it separate island: its ground still tied to
  CM4 through USB and PPS returns, and should be.
- **New BNC input on Si5351 enclosure (fed from DA R out 2): isolate it.** Y-cap RF bond
  can't be removed; DC/mains-frequency loop a bonded connector would add can be. Shield
  to board ground pour only.

**Two possible earth injections.** If AR-40A chassis reach mains PE, reference chain
earthed at both ends — AR-40A and Extron — and 10 MHz coax close loop. Test: unplug
AR-40A supply, DMM continuity from enclosure to earth pin of its mains inlet. `[?]`

**This not change the pad.** AR-40A output ground already its chassis ground, so
isolating connectors on pad hanging off that output achieve nothing — shield arrive
bonded. Bond pad enclosure, move on. Isolated bulkhead connectors only relevant for
boxes that introduce *new* earth reference, e.g. rack-mounted timing board with own
earthed supply.

**Priority judgement.** Loop current on 10 MHz shield put millivolts on 1.245 Vpp signal
— 60–70 dB down — into comparator inputs. Small periodic phase modulation at mains
frequency, few ps at 42 V/µs slew, average out over any useful measurement interval. Not
touch frequency accuracy. Ground noise on **PPS path** (F9T↔CM4) is the one that matter,
because it shift single edge that nothing average. Protect that island; don't spend
effort breaking 10 MHz loop until measurements justify it. Never lift safety earth for
either.

54 MHz coax alone is single clean bond, not loop. But because AR-40A output is T'd,
mains earth from Extron reach Si5351 island and then CM4 through chain of shields.
Whether current actually flow depend on whether CM4 side separately earthed.

**Consequence if it do:** loop current develop millivolts across ground that PPS
timestamping is referenced to. Not break anything; bias and slowly modulate threshold
crossing — look like small wandering offset, not fault.

**Measurements to settle it:** DMM on AC volts between (a) F9T ground and CM4 ground,
(b) CM4 ground and Extron chassis. More than few mV = loop current worth chasing. `[?]`

**Cable thermal note.** PTFE coax (RG-178, RG-316) have phase-vs-temperature
discontinuity ~19–21 °C, stepping delay by few hundred ppm over ~2 °C. For rig in room
crossing that point daily this is biggest thermal term in distribution. Solid or foam PE
behave better through room temperature. Most relevant on GNSS antenna feed — longest
run, only one seeing outdoor temperature.

---

## 6. Planned board `[plan]`

Replace ad-hoc T-and-breakout arrangement on digital side. JLCPCB, 4-layer.

**Fed from DA output at 1.245 Vpp** (7.01 dB pad, §4), therefore:

- **Input:** 75 Ω termination (not 50 Ω — downstream of DA now).
- **Squarer: comparator, mandatory.** At 1.245 Vpp biased at VDD/2 swing is 1.03–2.27 V,
  inside Si5351 V_IL (0.99 V) and V_IH (2.31 V) — reach neither threshold. 74AHC1GU04 no
  better placed. LTC6957-4 is purpose-built choice (sine-to-CMOS reference buffer,
  selectable input filtering, sub-ps additive jitter). ADCMP600 or LT1719 also work. CMOS
  inverter ruled out at this level.
- **Bias:** 100 nF C0G series, then 10k/10k to VDD/GND with 1 µF on tap. Use matched pair
  or array so tempcos track — ratio set threshold.
- **Supply:** dedicated ultra-low-noise LDO (ADP151 / TPS7A20 / LP5907), ferrite + 10 µF
  at input, not shared with anything switching. Matter more than temperature: CMOS
  threshold track VDD direct.
- **Outputs:**
  - squared 10 MHz → Si5351 CLKIN, 33 Ω series, receiver high-Z
  - ÷10⁷ chain → 1 PPS → ZED-F9T EXTINT (4 × 74HC390, or GreenPAK)
- **Layout:** ground plane under input, bias network tight to pin, pour both sides and
  stitch. Controlled impedance unnecessary at 10 MHz over few cm.

**Why ÷10⁷ output is the point of the board.** Rubidium-derived 1 PPS on EXTINT give
continuous UBX-TIM-TM2 time-marks of AR-40A against GNSS — running drift measurement,
replacing oscilloscope phase-drift method in AR-40A manual (§3.2.1: 1×10⁻¹⁰ = 100 ns
over 1000 s). Close measurement loop even while disciplining loop stay open.

**Thermal budget check.** At 1.245 Vpp, slew at zero crossing ~39 V/µs, so 1 mV of
threshold shift = ~26 ps of phase. Even 1 mV/°C over 20 °C room swing = ~510 ps static
offset, fractional excursion around 10⁻¹³ over hours. AR-40A own temperature spec is
±2×10⁻¹⁰. Conditioning stage orders of magnitude below thing it feed — can't affect
frequency accuracy, only slowly-varying phase.

**Alternative worth weighing:** feed board direct from AR-40A instead of from DA output.
Gain 2.22 Vpp (~1.7× better slew, proportionally less threshold-noise jitter) and remove
board ground from Extron mains earth. Cost: second output or splitter at rubidium.

---

## 7. Open items

Ordered by what block what.

### Closed

- ~~**Measure AR-40A open-circuit output and source impedance.**~~ 2026-09-07: 4.44 Vpp
  open, 46.2 Ω source, +11.25 dBm available.
- ~~**Fit the pad.**~~ 2026-09-07: DIY 7.01 dB pi, verified to 1% of prediction. Rebuilt
  2026-09-09 with same 150/51/150 values; confirmed indirectly by CLKIN measuring
  2.1 Vpp clamped from expected 2.49 Vpp, only reachable if pad deliver 1.245 Vpp into
  DA.
- ~~**Resolve AR-40A BIT polarity contradiction.**~~ Settled by existing lock interlock —
  see §3.
- ~~**Check what Si5351 breakout do at its SMA input.**~~ 2026-09-09: nothing. CLKIN sit
  at 0 V DC and clamp on negative half (§8), so no bias network on breakout.
- ~~**Verify chain end to end.**~~ 2026-09-09: AR-40A → pad → DA R in → DA R out → CLKIN
  unterminated. PLLA lock, CLK4 read 54.000 MHz, CM4 boot.
- ~~**Verify supply rails.**~~ 2026-09-09: all four within 23 mV, CM4 at 4.917 V booted
  (§5).

### Open — digital side, doable on bench

1. **Teensy firmware: gate CLK4 on Si5351 status.** Priority item now. With no CLKIN part
   free-run and emit ~11.6 MHz (§2b) — present CM4 with stable-looking wrong clock rather
   than none. Poll register 0 (LOS_CLKIN, LOL_A), hold CLK4 disabled when either set —
   periodically, not once at boot. Use BIT on Teensy pin 2 for *indication* (LED + serial
   to CM4), not for gating, to avoid reinstating 5-minute delayed boot. Build on working
   Etherkit sketch, not untested bare-register rewrite. Do before rack move.
2. **Confirm F9T breakout I/O level** is 3.3 V and not shifted to 5 V, before anything
   touch CM4 GPIO.
3. **Decide F9T data link:** fix USB grounding (cable GND to header GND pin, short;
   shield terminated at CM4 end only) or move to UART.
4. **Bring EXTINT to bulkhead connector** while back panel open; TIME1 to CM4 stay
   internal, direct, on miniature coax with 33–47 Ω series at F9T end.

### Open — need rack move, or AR-40A powered down

5. **Verify DA output is clean sine into real 75 Ω load.** Measured clean unloaded (§3),
   which rule out voltage clipping and slew limiting. Still untested: current drive into
   75 Ω. *Gate the board design* — if DA sag, board input level change and Extron role
   must be reconsidered. Mitigate by laying out footprints for either 75 Ω or 50 Ω
   termination, jumper-selected, so board can be ordered without waiting.
6. **Confirm Orion actual AC termination.** Same measurement as item 5: T at Orion input,
   read Vpp with Orion connected vs disconnected. ~1.26 / ~2.5 Vpp = terminate at 75 Ω.
7. **AC volts between grounds** — F9T↔CM4, CM4↔Extron chassis. Take F9T↔CM4 reading with
   10 MHz coax disconnected as well as connected; difference is actual size of earth-path
   effect.
8. **Is AR-40A chassis at mains PE?** Unplugged continuity check, enclosure to inlet earth
   pin. Determine whether 10 MHz chain have one earth injection or two (§5).
9. **Antenna:** confirm where coax shield bonded; if roof-mounted, gas-discharge arrestor
   at entry point. GNSS DC block / galvanic isolator remove whole class of ground problem,
   provided it pass active antenna bias.

### Shopping list

- BNC tees — none on bench.
- 75 Ω BNC feedthrough terminator — SDS1104X-E is 1 MΩ fixed, no switchable 50 Ω.
- SMA attenuator kit (1/2/3/6/10 dB), optional but useful for bench work.

---

## 8. Things known out of spec but working

Kept here so nobody "fix" them without thought.

- **54 MHz into CM4 XIN at full 3.3 V CMOS swing** through 10 Ω. Crystal inputs generally
  want ~1 Vpp AC-coupled. Stable for years. For PPS timestamping accuracy this path close
  to irrelevant anyway — few ps of jitter on CPU clock disappear under interrupt latency.
  Matter for boot stability and system clock frequency accuracy at long tau, not for edge
  capture.
- **AR-40A into Extron at 3.02 Vpp** vs 1.5 Vpp max. Being fixed by pad.
- **Zero-centred sine into Si5351 CLKIN — CONFIRMED BY MEASUREMENT 2026-09-09.** `[meas]`
  Fed 2.49 Vpp unterminated from DA output, CLKIN measure **2.1 Vpp / 776 mV rms, visibly
  asymmetric**. Predicted clamping: positive peak sit near +1.25 V unclamped while
  negative held around −0.85 V by input protection diode plus source impedance, summing
  to 2.1 Vpp instead of 2.49. Crest factor 2.705 vs 2.828 for clean sine — one half
  compressed.

  So protection diode conduct every cycle and act as level shifter, against −0.5 V
  absolute maximum. Functionally worked for years, and Si5351 own PLL contribute far more
  jitter than this. Still worth fixing on new board, two reasons: sustained current
  through protection diode not what it is for, and threshold crossing set by diode forward
  characteristic, which drift with temperature rather than sit at fixed bias. This is
  measured justification for §6 input stage.
