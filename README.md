# 10 MHz Reference & Timing Rig — Architecture

Status 2026-09-11. Written from rig already running. Doc say what exist, what out of
spec, what still unmeasured.

**Confidence marks:**
- `[built]` — how wired, direct account
- `[meas]` — measured on this hardware, with date
- `[spec]` — datasheet/manual, not verified here
- `[calc]` — derived from `[spec]` numbers
- `[?]` — unverified, need measurement
- `[plan]` — not built yet

---

## 1. Purpose

Two independent goals, one rubidium:

1. **Frequency reference.** Short-term stability + holdover for audio clock
   (Orion 32 HD) and bench instruments.
2. **Time reference.** UTC-traceable absolute time on CM4, which run chrony as
   stratum-1 NTP/PTP source.

Complementary, not redundant: AR-40A give stability, ZED-F9T give traceability.
GPSDO architecture split across separate boxes, disciplining loop currently open
(see §7).

Explicitly **not** goal: Dante or AES67 grandmaster.

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
              Teensy 4.0 ── I2C (18/19) ──────┤
              (BIT lock → pin 2)              │
              Serial1 (0/1) → CM4             │
              ttyAMA5, GPIO12/13              │
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
                  1 PPS (TIME2) ─┘       └── UART1, 38400
                  47 Ω, coax, J2                 ttyAMA0
                  pin 9 = SYNC_OUT               GPIO14/15
                             │                      │
                        ┌────┴──────────────────────┴───┐
                        │  u-blox ZED-F9T               │
                        │  breakout                     │
                        └───────────────────────────────┘
                              │            │ SMA
                          EXTINT           ▼
                        BNC present,  GNSS antenna
                        unconnected
                        (awaits new
                         board) `[plan]`
```

Power: one MeanWell RS-15-5 (5 V), three separate runs from its terminals — CM4,
Si5351/Teensy, ZED-F9T. AR-40A on own LRS-75-15 (15 V), −V deliberately not tied to
5 V supply. See §5.

**Superseded arrangement**, kept so not reintroduced: AR-40A output used to be split
by passive soldered T, unpadded, feeding Extron and Si5351 CLKIN directly. Put
2.75 Vpp into 1.5 Vpp-max input, gave two destinations no isolation from each other.
Replaced by pad + one DA output per destination.

---

## 2b. Lock interlock — relay removed, replaced in firmware `[built]`

Previously: AR-40A BIT pin (DB9 pin 3) drove relay coil — coil between +V and BIT pin,
flyback diode across it — relay switched 5 V to CM4, Teensy and Si5351 board, LED
showed same state. Ran for years. **Removed; all three now power up directly.**

AR-40A 10 MHz output present from power-up; specified 5 minutes is time to *lock*, not
time to output. So interlock never required for CM4 to boot.

Two consequences of removing it:

- **Warm-up frequency step.** Si5351 now locks PLLA to free-running OCXO (~10⁻⁷ off)
  at power-up, CM4 boots on that, frequency shifts as physics package pulls in ~5 min
  later. chrony copes but re-converges frequency estimate after every boot.
- **No clean failure on dead rubidium — MEASURED 2026-09-09.** `[meas]` With 5 V
  applied, 10 MHz BNC open, CM4 unpowered, CLK4 produce periodic **~11.6 MHz** output
  (counter; confirmed by eye at 20 ns/div), 1.72 Vpp / 686 mV rms into CM4 clamps.
  PLL loop open, charge pump rails, VCO parks ~186 MHz — MS4 ÷16 give 11.6 MHz. The
  600–900 MHz VCO figure is specified range *with* reference, not physical bound.

  Worst of three possible outcomes: not silence, not obviously broken, but
  stable-looking clock at ~fifth of nominal. CM4 internal PLLs configured around
  54 MHz, likely won't lock at all, so symptom is board with power that does nothing.
  Hard to diagnose cold.

  Amplitude and frequency both contaminated by unpowered CM4 input clamps conducting —
  with CM4 powered, frequency unchanged at 11.6 MHz, amplitude rise to 2.36 Vpp /
  ~896 mV rms as clamps stop conducting. `[meas]` (Still below full 3.3 V CMOS swing
  because scope 100 MHz bandwidth rounds edges; crest factor 2.63 sits between square
  and sine — signature of that filtering.)

  **CM4 symptom with invalid XIN clock, confirmed `[meas]`:** power and green LEDs
  lit, static, no activity flicker. SoC internal PLLs never lock at 11.6 MHz so boot
  ROM doesn't run. Field diagnostic — two LEDs lit, no activity = no valid clock at
  XIN.

**Makes Teensy lock-gating necessary, not optional.** Holding CLK4 disabled until BIT
read locked converts this case into clean no-clock. Drive LED from BIT state at same
time → positive "reference not locked" indication — same info old relay LED gave,
without relay, better than inferring from Pi that won't boot.

**Replacement, decided 2026-09-08, wired 2026-09-09 `[built]`:** BIT → **Teensy GPIO
pin 2**, `INPUT_PULLUP`, LOW = locked. Teensy gates CLK4 on Si5351's own LOS_CLKIN /
LOL_A status, use BIT for indication only (LED + serial to CM4), so boot not
conditional on AR-40A five-minute warm-up. Nothing in 5 V path. Firmware: `5351.ino`.

- **Teensy 4.0 pins NOT 5 V tolerant** — 3.3 V absolute max. BIT is open collector,
  only sinks, so safe *provided nothing else on that line*. Old interlock had relay
  coil between +V and BIT; if any of that wiring remains, open BIT pulled up through
  coil to relay supply and destroys input. **Verify coil out of circuit before
  powering.**
- 1 kΩ series at pin for ESD, 100 nF pin-to-ground to keep RF off it.
- Pin 2 chosen: no alternate peripheral function on Teensy 4.0. Pin 10 considered
  first but is SPI CS.

**Panel LEDs, wired 2026-09-09 `[built]`:** yellow on pin 3 = REF (solid when CLKIN
present and PLLA locked, blink while PLL settles, off when no signal); red on pin 4 =
LOCK (on when AR-40A reports locked). Onboard pin 13 = heartbeat. Both LEDs ~1.5 mA
through 1 kΩ — Teensy 4.0 pins default ~4 mA drive, don't push. Measured Vf: yellow
1.82 V, red 1.794 V.

**Wiring checked and both LEDs operating, 2026-09-10** `[meas]`. An earlier session saw
them dark with otherwise-healthy status; that was wiring, not firmware, and it is
resolved. 1.5 mA through 1 kΩ is enough to see.

Wiring: one 2-conductor shielded multicore strand — two conductors carry anodes,
shield is common cathode return to Teensy ground. Resistors at Teensy end. Shield must
not also touch chassis; here it is conductor, not shield, and bonding it to enclosure
would create the loop §5 avoids.

Two LEDs distinguish failure modes old single LED could not: REF off = broken signal
path (cable, DA or AR-40A); REF solid + LOCK off = working path on undisciplined free
OCXO.

Colour convention inverted from habit — red lit means *good* here. Alternative: drive
LOCK inverted so red means "unlocked or holdover", making dark panel + yellow the
healthy state.

**Optocoupler: not needed — earlier advice in this doc was wrong.** Rule about second
inter-domain paths is about *low-impedance* ones: shield bond is milliohms and carries
real current. This path sits behind pull-up of tens of kΩ, so carries only microamps,
and the 10 MHz coax shield already bonds those grounds at low impedance anyway.
High-impedance path alongside existing low-impedance bond changes nothing.

One case that would change it: if AR-40A ground and Teensy ground differed by more
than few hundred mV, saturated BIT transistor would pull pin below Teensy ground and
forward-bias its ESD diode. Shield ties them, so expect millivolts — confirmed by
AC-volts check in §7. `[?]`

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
load; does not state output *is* 50 Ω source. Measured 46.2 Ω — see below — so figures
above hold.

**Source impedance: measured 46.2 Ω** `[meas]` — 4.44 Vpp open, 2.24 Vpp across
measured 47 Ω terminator (10 cm coax), Zs = R × (V_open − V_L)/V_L. 50 Ω source within
measurement error. Available power +11.25 dBm, inside +12±2 dBm spec. Crest factor
consistent open and loaded (2.87 / 2.90 vs 2.828 ideal) — ~2.5% excess is systematic
peak-detect noise, not load-dependent distortion.

DMM DC resistance at SMA reads ~84 Ω. `[meas]` Does **not** establish RF source
impedance — DC resistance and impedance at 10 MHz are different quantities. Does rule
out transformer- or capacitor-coupled output (either would read open), so DC-conductive
resistive network to centre pin exists. Superseded by AC measurement above; kept only
to record the two disagree.

**Manual inconsistency — RESOLVED.** AR-40A manual state BIT polarity two contradicting
ways:

- §1.3 spec table and §2.1.2: locked = pin 3 shorted to ground ("0")
- §3.3: "0" logic described as *open collector* = lock; "1" = *short to ground* = unlock

**§2.1.2 correct; §3.3 has labels swapped.** `[built]` Resolved empirically by existing
interlock circuit: relay coil between +V and BIT pin, flyback diode across coil, CM4
powers up *after* lock. Pin therefore sinks coil current when locked. Ran for years.

Constraint worth recording: AccuBeat do not specify BIT open collector sink current.
Adequate for present relay coil, but undocumented rating — if relay replaced with
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

Revises earlier note: the ~2% crest-factor excess on all probe measurements was the 10×
probe, not scope systematic. Coax direct is the better measurement here.

**Usage constraints:**
- Use **R, G or B** planes (or Y on a YUV A). H/V sync inputs are 510 Ω into TTL
  squarer spec'd 15–180 kHz — useless at 10 MHz.
- On DA6 YUV A, avoid digital audio BNC: 510 Ω input, 2.5 Vpp output.
- DIP switches: Gain/Peak **off** (no shaping on reference sine). AC coupling is safer
  default.
- R, G and B are independent signal paths sharing one chassis, so 6-output unit fed on
  R leaves G and B as two more idle 1→6 buses.

### Antelope Orion 32 HD — 10M input
| Parameter | Value | Src |
|---|---|---|
| Signal | sine | `[built]` |
| Impedance | 75 Ω | `[spec]` |
| Level | 1 Vpp nominal | `[spec]` |
| Front end | comparator / squarer, self-biasing | `[spec]` |
| Actual AC termination | unconfirmed | `[?]` |

Every Orion clock mode disciplines the same internal OCXO. Advantage of 10M over word
clock is multiplication ratio (~×2.26 vs ×512), i.e. far less phase-noise
amplification. 10M is the right input.

### Si5351C-B breakout + Teensy 4.0
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
| Teensy Serial1 | pin 0 = RX1, pin 1 = TX1, 115200 8N1 → CM4 ttyAMA5 | `[built]` |

**Teensy → CM4 status link** `[built]` 2026-09-09. Teensy pin 1 (TX1) → CM4 40-pin
header pin 33 (GPIO13, RXD5); pin 0 (RX1) ← header pin 32 (GPIO12, TXD5); GND to header
pin 34. Needs `dtoverlay=uart5`; port is `/dev/ttyAMA5`. Sketch reports go to small
`Print` subclass writing to both USB and Serial1, so bench debugging unchanged and CM4
gets same text — reference state, PLL lock, CLK4 state, AR-40A BIT, I2C error count,
every ~2 s and on every BIT change. Nothing read from RX1 yet; wired for future command
channel.

Measured **0.6 mV** between Teensy ground and CM4 ground after adding this second bond
between islands (first being the 54 MHz coax). `[meas]`

Both PL011s report `irq = 35` — F9T NMEA and Teensy status text share an interrupt
line. Keep Teensy message rate low.

Two `.ino` files in one sketch folder get concatenated into single translation unit by
Arduino toolchain, so stray `si5351c_clkin_54mhz.ino.ino` alongside the real one
produces wall of "redefinition of ..." errors starting at first symbol. Primary sketch
filename must match folder name.

**Do not** feed 10 MHz into XA — that input specified 25–27 MHz only.

Breakout has own onboard regulator `[built]`, so Si5351 VDD — and therefore CLKIN
thresholds — already decoupled from CM4 transients on shared 5 V rail. Part number
unidentified, so PSRR above ~10 kHz unknown. `[?]` Does not remove need for dedicated
LDO on new board (comparator threshold tracks *its* supply), nor star-grounding point
in §5 — regulator holds VDD steady against its own ground pin, does nothing about
ground currents moving that reference relative to incoming coax shield.

### Raspberry Pi CM4
- BCM2711 crystal removed; 54 MHz injected at XIN from Si5351 CLK4. `[built]`
- Out of spec on paper (crystal inputs generally want ~1 Vpp AC-coupled; 10 Ω series
  into high-Z XIN passes essentially full 3.3 V CMOS swing), but stable in service for
  years. `[built]`
- Dedicated realtime Linux. `[built]`
- chrony, stratum-1 NTP/PTP source. `[built]`
- Powered from MeanWell RS-15-5 shared with Teensy and Si5351 board. `[built]` 15 W /
  3 A at 5 V — check headroom, CM4 alone can take over half under load. `[?]`
- **Carrier: standard Raspberry Pi CM4 IO Board.** 5 V fed into J20 (4-pin Berg). Pin
  assignment verified by continuity on this board: **J20 pin 3 = GND, pin 4 = +5 V.**
  Nothing on J19. `[meas]`
- **L5 removed** `[built]` — documented mod for feeding external 5 V. Stops onboard 5 V
  and 3.3 V supplies starting and keeps 5 V off DC jack. Consequence: no 12 V rail, so
  PCIe slot can't power a card needing it and fan header may be dead. Don't reinstate
  L5 without reconsidering supply.
- 40-pin header 5 V pins are same net as J20's — feeding either backfeeds onboard
  supply identically. Header is not a way around L5.

**J2 is the 14-pin configuration header, not the 40-pin GPIO header.** Jumper across
pins 1–2 is `nRPI_BOOT`, 3–4 is `EEPROM_nWP`. Reference layout: 1 GND, 2 nRPIBOOT,
3 GND, 4 EEPROM_nWP, 5 AIP0, 6 AIP1, 7 GND, 8 SYNC_IN, 9 SYNC_OUT, 10 GND, 11 TVDAC,
12 GND, 13 RUN_PG, 14 GLOBAL_EN.

**PPS goes to pin 9, the one labelled SYNC_OUT.** `[meas]` Label describes PHY role in
1588 network, not direction of your wire. Driver exposes exactly one PTP pin, names it
`SYNC_OUT`, and `/sys/class/ptp/ptp0/pins/SYNC_OUT` reads `1 0` — function 1 =
`PTP_PF_EXTTS`, channel 0. Pin 8, labelled SYNC_IN, reported by others as not correctly
wired on CM4IO. Jumper already sat on pin 9 from earlier build; that was the answer all
along.

**PHY hardware timestamping works on this install.** `[meas]` `ethtool -T eth0` reports
hardware-transmit, hardware-receive and PTP Hardware Clock 0. Not standard — BCM54210PE
driver came from Timebeat on Raspberry Pi's behalf and needs out-of-tree kernel build.
**Makes this rootfs hard to reproduce** — the whole argument for the image below and
against a rolling distro.

**Timing software chain, as configured** `[built]`: chrony reads extts events itself via
`refclock PHC /dev/ptp0:extpps` — no `ts2phc` involved, no unit for it exists.
`phc2sys -s CLOCK_REALTIME -c eth0` pushes disciplined system clock *out* to PHC for PTP
server side. The intended `refclock SHM 0` NMEA source is currently commented out and
must be restored after the antenna is reattached; `hwtimestamp eth0` remains configured.
Upstream NTP is mikes.fi.

**Deliberate tuning in config.txt worth carrying forward:** `force_turbo=1` pins core
clock (mini-UART baud derives from it), `dtparam=eee=off` plus `genet.eee=N` on kernel
command line disable Energy Efficient Ethernet, which otherwise wrecks PTP latency.
`dtoverlay=miniuart-bt` and `dtoverlay=disable-bt` were both present and contradictory;
**`miniuart-bt` removed 2026-09-10**, `disable-bt` kept. `dtoverlay=uart5` added
2026-09-09 for Teensy link.

**eMMC imaged 2026-09-09** `[built]` via `rpiboot -d mass-storage-gadget64` from Arch
laptop, jumper on J2 1–2, micro-USB to J11, ~40 MB/s. Two obstacles worth remembering:
**usbguard** blocks device when it re-enumerates after first boot stage, showing as
"Device located successfully" then `op_get_active_config_descriptor: device
unconfigured` and segfault — stopping daemon does not undo existing block, device must
be allowed or policy set permissive. And `mass-storage-gadget64` files live in usbboot
repo, so build from source rather than relying on packaged `rpiboot`.

**No-boot diagnostics, learned 2026-09-10 the long way.** Whole session went into CM4
that would not boot; cause was the Ethernet cable. Clock, both firmware versions,
filesystems and F9T-on-console theory all suspected, all innocent. Shortcuts that would
have found it in minutes:

- **If `rpiboot` enumerates the eMMC, XIN is good.** BCM2711 boot ROM runs off XIN, so
  device appearing on laptop proves whole clock chain — AR-40A, pad, DA, Si5351, CLK4 —
  in one step, no scope. First thing to try, not last.
- **A genet or PHY error means it booted.** "eth0 failed to connect PHY" comes from
  kernel, so ROM, bootloader, kernel and userspace all ran. Any kernel message at all is
  proof of boot.
- **J2 1–2 (`nRPI_BOOT`) must come off after imaging.** Left on, CM4 goes to USB boot
  mode every time and looks identical to dead board.
- **FAT dirty bit after imaging session is souvenir, not damage.** `fsck.fat` reporting
  boot-sector difference at offset 65 *is* the dirty flag — one finding, not two.
  Disable desktop automount before attaching gadget: automounting ext4 replays journal,
  which is a write, the plausible route by which read-only imaging session dirties
  anything.
- **Incoming UART traffic cannot hang a Pi boot.** No boot stage reads the console. Baud
  mismatch makes healthy boot *look* dead on a terminal — different problem, see
  boot-console item in §7.

### u-blox ZED-F9T on breakout

- I/O is 3.3 V. Confirmed indirectly 2026-09-09: breakout TX drives GPIO15 cleanly and
  CM4 decodes it, which a 5 V-shifted output would not do without conducting through
  input protection diode. `[meas]`
- Header: +5V, GND, RX(2)/TX(2), TIME(2)/TIME(1), EXTINT, READY, SCL/SPI_CLK,
  SDA/SPI_CS, USB D−/D+, RX/SPI_MOSI, TX/SPI_MISO. SEL pad selects UART+I²C vs SPI.
- **Data link: UART1, not USB.** `[built]` 2026-09-09. F9T TX/SPI_MISO → CM4 GPIO15
  (header pin 10, red), CM4 GPIO14 (header pin 8, white) → F9T RX/SPI_MOSI. 38400 8N1
  on **/dev/ttyAMA0** — the PL011, reached via `dtoverlay=disable-bt`. NMEA confirmed
  flowing. USB dropped deliberately: breakout ties header +5V and USB VBUS, so cable
  from CM4 would have paralleled RS-15-5, and USB on CM4 sits behind VL805 over PCIe.
  The stale ser2net mapping to this UART was disabled 2026-09-10: u-center access now
  requires an explicit maintenance window rather than silently contending with gpsd.
- **1 PPS: TIME2 → CM4 J2 pin 9.** `[built]` 2026-09-09. Miniature coax, 47 Ω series at
  F9T end, **shield grounded at CM4 end only** (see below). chrony sees pulses.
- **TIME1 free**, deliberately: if UBX-TIM-TP reports quantisation error for TP1 only on
  this firmware, TP1 is wanted for that. Also the scope reference when ÷10⁷ output
  arrives.
- **EXTINT already brought out to BNC** near F9T `[built]`, currently unconnected and
  **not galvanically isolated**. Awaits ÷10⁷ output from planned board for UBX-TIM-TM2
  time-marking. `[plan]` Isolate that path later only if ground problem actually appears
  on it — no panel work needed to use it.

**Grounding PPS shield at both ends killed the F9T.** `[meas]` 2026-09-09. With shield
bonded at F9T end as well as J2, module stopped responding on UART entirely; lifting it
at F9T end restored everything immediately. First read was pin contention against PHY
output — wrong. Measured afterwards: **<10 mV** between F9T ground and CM4 ground with
single bond, so steady-state potential was never the problem, and mechanism remains
unexplained. Recorded rather than solved; do not reintroduce second bond casually.

**TIME2 pulse measured at CM4 end** `[meas]` 2026-09-09: 3.32 Vpp, 18 ns rise. Settles
documentation conflict: the pad is 3.3 V. CM4 datasheet says 3.3 V in §2.2 and its pin
list, but older revision and CM4IO schematic net labels both say 1.8 V. No level
translation needed.

RMS figure from same session (2.18 V, implying ~43% duty) was scope measurement-window
artefact, not module config. Disregard it.

---

## 4. Levels

Everything downstream set by one number: AR-40A actual output, which has ±2 dB spec
spread.

**As built (no pad):** 4.44 Vpp EMF from 46.2 Ω source into DA 75 Ω = **2.75 Vpp**, i.e.
1.83× DA 1.5 Vpp maximum. DA has been running overdriven. `[calc]`

**Pad: built and verified.** `[meas]` DIY 50 Ω pi, measured values 150.000 Ω / 50.975 Ω
/ 149.337 Ω = **7.01 dB**. Mounted between AR-40A SMA and output BNC. Verification
2026-09-07: 992 mV–1.0 Vpp across measured 47 Ω terminator, against 998 mV predicted —
within 1%. Crest factor 2.89, matching 2.87 / 2.90 measured open and loaded before pad,
so ~2% excess over 2.828 is instrument systematic, not distortion.

Rebuilt 2026-09-09 (series and shunt parts replaced). DC port resistance with far port
open: **85.644 Ω input, 85.565 Ω output** `[meas]`, against 85.75 Ω predicted for
150 ∥ (51 + 150). Network intact and symmetric to 0.09%. Not the same quantity as the
55.7 Ω terminated input impedance — open-port DC resistance and terminated input
impedance are different measurements of same network. Also open-port resistance does not
uniquely determine attenuation (120/179/120 reads similarly and is much larger pad), so
re-run functional check when AR-40A supply returns: 4.44 Vpp in, expect ~1.0 Vpp across
47 Ω terminator. `[?]`

Resulting levels:

| Destination | Level | Note |
|---|---|---|
| DA input | 1.245 Vpp | 17% margin under the 1.5 Vpp max |
| Orion (75 Ω) | 1.245 Vpp | 25% over 1 Vpp nominal; comparator input |
| Bench instrument (50 Ω) | 0.996 Vpp ≈ +3.9 dBm | 2 × V × 50/125 |
| New timing board (75 Ω) | 1.245 Vpp | see §6 |
| Impedance seen by AR-40A | 60.4 Ω, ~20 dB return loss | vs 75 Ω unpadded |

Keep DA gain DIP at unity, not +1.1 dB.

**Pad enclosure grounding:** irrelevant either way. AR-40A output ground is its chassis
ground, which is earthed, so pad sits inside already-earthed path — isolating its
connectors from its own box changes nothing (§5).

**Supply.** Measured 1.3 A @ 15 Vdc warm-up, settling sharply to **0.600 A in under
5 minutes** — matches 0.6 A steady-state spec and 5 min warm-up figure exactly. Oven
reaching setpoint promptly; no sign of aging lamp or struggling thermal loop. Unit
healthy. `[meas]`

19.5 W warm-up / 9 W steady, so size supply for 2 A and respect manual's ≤1 °C/W
heatsink recommendation — 9 W in small box feeds straight into ±2×10⁻¹⁰ temperature
spec.

---

## 5. Grounding topology

Ground paths currently existing between subsystems:

| Path | Notes |
|---|---|
| AR-40A module case → enclosure chassis | bonded `[built]` |
| AR-40A SMA output ground → chassis | bonded — **the reference chain's signal ground is the AR-40A chassis** `[built]` |
| AR-40A PSU ground → chassis | bonded `[built]` |
| AR-40A chassis → mains PE | **bonded — continuity confirmed 2026-09-10** `[meas]` |
| Extron chassis → 10 MHz coax shields | Extron has a grounded IEC inlet — **mains earth enters the reference chain here** |
| AR-40A T → Si5351 board ground | via coax shield |
| Si5351 board → CM4 | 54 MHz coax shield, grounded both ends |
| Teensy + Si5351 + CM4 supply | **all three on one MeanWell RS-15-5 (5 V)** — they share a DC ground through the supply wiring `[built]` |
| AR-40A supply | MeanWell LRS-75-15 (15 V), separate — **−V deliberately not tied to the 5 V supply's −V** `[built]` |
| F9T → CM4 | shared 5 V return + UART ground; PPS shield bonded at CM4 end only |
| GNSS antenna coax shield | antenna reattached 2026-09-10; bonding point still unknown `[?]` |
| Supply grounds | topology unconfirmed `[?]` |

**Correction to "floating island" framing.** Digital island is *not* floating once
10 MHz coax connected. Shield must bond to Si5351 board ground to serve as signal
return, and that shield runs back to earthed Extron. So island is earth-referenced
through coax shield, unavoidably. Isolating BNC from its enclosure prevents a *second*
path via chassis and rack; cannot isolate shield from board ground.

Goal is therefore **one path between domains**, not floating island. That path is the
coax shield.

Follows: **do not tie LRS-75-15 −V (AR-40A supply) to RS-15-5 −V.** 15 V supply −V sits
at AR-40A chassis, which is earthed; digital island reaches same earth via Extron and
coax shield. Bonding them adds second path and closes bench-sized loop. The two domains
exchange no signal needing shared DC reference — only 10 MHz coax, which carries its own
return, and interlock relay contacts, which are galvanically isolated. `[built]`

**Correction (2026-09-07).** Earlier draft treated Teensy/Si5351 board and CM4 as
separate ground islands bonded only by 54 MHz coax shield. Wrong: all three run from one
RS-15-5, so they share DC ground through supply wiring. Coax shield plus supply return
therefore form real loop, area set by cable routing.

Consequences and mitigations:

- **Route 54 MHz coax alongside DC supply pair.** Same connections, minimal enclosed
  area, no cost.
- **Star the DC distribution from PSU terminals**, separate pairs per board, not
  daisy-chained. Matters more than the loop: CM4 draws >1 A with fast load transients
  while Si5351 draws tens of mA. Shared return conductor puts CM4 CPU activity onto the
  ground that Si5351 CLKIN threshold is referenced to. Exception: Teensy chains off
  Si5351 board, which is correct — physically attached, draw small and near-constant,
  and separate run back to PSU would enclose larger loop between their grounds than it
  removes.
- **Rails measured at far end of each cable 2026-09-09** `[meas]`: CM4 4.936 V idle /
  **4.917 V booted under load**, Teensy 4.932 V, Si5351 4.937 V, F9T 4.955 V. 23 mV
  spread across four, ~60 mV total drop from nominal — flat readings confirming star
  topology. All comfortably above 4.75 V threshold; no leg needs paralleling.
- **Cable:** audio multicore inner pairs work — hot to +5 V, cold to return (already
  twisted, minimal loop area), shield bonded to −V at PSU end only, open at board. Do not
  use shield as current return. Watch CM4 leg: 24–26 AWG gives 84–134 mΩ round trip over
  0.5 m, i.e. 126–201 mV drop at 1.5 A. Measured as built: 4.917 V at CM4 under load —
  adequate, no action needed. `[meas]`
- **RS-15-5 earth behaviour: confirmed isolated.** `[meas]` Infinite resistance between
  FG terminal and DC output, so output floats galvanically. Only coupling to earth is
  Y-capacitor (few ohms at 10 MHz, megohms at mains frequency).
- **DC distribution as built `[built]`:** three separate 5 V runs from PSU terminals —
  CM4, Si5351/Teensy pair, ZED-F9T. F9T separate run keeps CM4 load current off its
  reference but does not make it separate island: its ground still tied to CM4's through
  USB and PPS returns, and should be.
- **New BNC input on Si5351 enclosure (fed from DA R out 2): isolate it.** Y-cap RF bond
  can't be removed; the DC/mains-frequency loop a bonded connector would add can be.
  Shield to board ground pour only.

**Two earth injections — CONFIRMED 2026-09-10.** `[meas]` Unplugged continuity from
AR-40A enclosure to its mains inlet earth pin: continuous. So reference chain is earthed
at both ends — AR-40A and Extron — and the 10 MHz coax shield closes that loop.

Loop is real, but measured to be harmless: CM4 chassis to Extron chassis reads **<3 mV
AC** (below), which is the voltage the loop actually develops across the grounds that
matter. Consistent with the priority judgement below — do not lift either earth, do not
chase this further until a measurement says otherwise.

**Does not change the pad.** AR-40A output ground is already its chassis ground, so
isolating connectors on a pad hanging off that output achieves nothing — shield arrives
bonded. Bond pad enclosure and move on. Isolated bulkhead connectors only relevant for
boxes introducing a *new* earth reference, e.g. rack-mounted timing board with own
earthed supply.

**Priority judgement.** Loop current on 10 MHz shield puts millivolts on 1.245 Vpp signal
— 60–70 dB down — into comparator inputs. Small periodic phase modulation at mains
frequency, few ps at 42 V/µs slew, averaging out over any useful measurement interval.
Does not touch frequency accuracy. Ground noise on **PPS path** (F9T↔CM4) is the one that
matters, because it shifts a single edge that nothing averages. Protect that island;
don't spend effort breaking the 10 MHz loop until measurements justify it. Never lift a
safety earth for either.

54 MHz coax on its own is single clean bond, not loop. But because AR-40A output is T'd,
mains earth from Extron reaches Si5351 island and then CM4 through chain of shields.
Whether current actually flows depends on whether CM4 side is separately earthed.

**Consequence if it does:** loop current develops millivolts across the ground PPS
timestamping is referenced to. Doesn't break anything; biases and slowly modulates the
threshold crossing — looks like small wandering offset rather than fault.

**Measured 2026-09-09** `[meas]`: F9T ground to CM4 ground <10 mV; Teensy ground to CM4
ground 0.6 mV. Both bonds benign in steady state.

**Measured 2026-09-10, in the rack** `[meas]`: CM4 ground to Extron chassis **<3 mV AC** —
the last missing reading, and the one where mains earth from the Extron inlet would have
shown up. It does not. Both earth injections exist (AR-40A chassis is at PE, above) and
the loop through the 10 MHz shield is therefore closed, but it develops single-digit
millivolts, not the tens or hundreds that would put a wandering bias on the PPS
threshold. Earth-path question is closed for this build.

Also: grounding PPS coax shield at *both* ends stopped F9T dead (§3) — fault the
millivolt readings do not explain, and reason to treat added bonds between islands as
changes worth testing rather than tidying.

~~**Measurements to settle it:** DMM on AC volts between (a) F9T ground and CM4 ground,
(b) CM4 ground and Extron chassis.~~ Both taken — <10 mV and <3 mV respectively. No loop
current worth chasing. `[meas]`

**Cable thermal note.** PTFE coax (RG-178, RG-316) has phase-vs-temperature
discontinuity around 19–21 °C, stepping delay by few hundred ppm over ~2 °C. For a rig
in a room crossing that point daily this is the largest thermal term in distribution.
Solid or foam PE behaves better through room temperature. Most relevant on GNSS antenna
feed — longest run, only one seeing outdoor temperature.

---

## 6. Planned board `[plan]`

Replaces ad-hoc T-and-breakout arrangement on digital side. JLCPCB, 4-layer.

**Fed from DA output at 1.245 Vpp** (7.01 dB pad, §4), therefore:

- **Input:** 75 Ω termination (not 50 Ω — downstream of DA now).
- **Squarer: comparator, mandatory.** At 1.245 Vpp biased at VDD/2 the swing is
  1.03–2.27 V, inside Si5351 V_IL (0.99 V) and V_IH (2.31 V) — reaches neither
  threshold. 74AHC1GU04 no better placed. LTC6957-4 is the purpose-built choice
  (sine-to-CMOS reference buffer, selectable input filtering, sub-ps additive jitter).
  ADCMP600 or LT1719 also work. CMOS inverter ruled out at this level.
- **Bias:** 100 nF C0G series, then 10k/10k to VDD/GND with 1 µF on tap. Use matched pair
  or array so tempcos track — ratio sets threshold.
- **Supply:** dedicated ultra-low-noise LDO (ADP151 / TPS7A20 / LP5907), ferrite + 10 µF
  at its input, not shared with anything switching. Matters more than temperature: CMOS
  threshold tracks VDD directly.
- **Outputs:**
  - squared 10 MHz → Si5351 CLKIN, 33 Ω series, receiver high-Z
  - ÷10⁷ chain → 1 PPS → ZED-F9T EXTINT (4 × 74HC390, or a GreenPAK)
- **Layout:** ground plane under input, bias network tight to pin, pour both sides and
  stitch. Controlled impedance unnecessary at 10 MHz over few cm.

**Why ÷10⁷ output is the point of the board.** Rubidium-derived 1 PPS on EXTINT gives
continuous UBX-TIM-TM2 time-marks of AR-40A against GNSS — running drift measurement,
replacing oscilloscope phase-drift method in AR-40A manual (§3.2.1: 1×10⁻¹⁰ = 100 ns
over 1000 s). Closes measurement loop even while disciplining loop stays open.

**Thermal budget check.** At 1.245 Vpp, slew at zero crossing ~39 V/µs, so 1 mV of
threshold shift = ~26 ps of phase. Even 1 mV/°C over 20 °C room swing is ~510 ps static
offset, fractional excursion around 10⁻¹³ over hours. AR-40A own temperature spec is
±2×10⁻¹⁰. Conditioning stage is orders of magnitude below the thing it feeds — can't
affect frequency accuracy, only slowly-varying phase.

**Alternative worth weighing:** feed board directly from AR-40A instead of DA output.
Gains 2.22 Vpp (~1.7× better slew, proportionally less threshold-noise jitter) and
removes board ground from Extron mains earth. Costs second output or splitter at
rubidium.

**Scope question — settle before layout.** Three apparently separate choices are one
decision:

1. Whether squarer, Si5351 and Teensy share one enclosure (§6b consolidation).
2. Whether board is fed from DA output or straight from rubidium (above).
3. Whether CM4 XIN gets proper receiver circuit at its end instead of bare 10 Ω into
   clamping input (§8).

(3) is **deferred until a second identical CM4 module is available** to experiment on —
running rig is not the place to characterise that input, and the answer is a circuit at
the far end, not a probe reading. So it won't be resolved on this board's schedule. But
board should be laid out knowing it is coming: treat 54 MHz path as driver into defined
load rather than as wire, and leave series-element footprint able to take something other
than a single 10 Ω.

Item 8 in §7 no longer blocks ordering: DA behaviour into 75 Ω gets measured at rack
move, and jumper-selectable 75/50 Ω input termination covers either outcome.

---

## 6b. ZED-F9T wiring — built 2026-09-09

Full detail in §3. Summary of what was decided and what it cost:

**Built:**

- **UART1 → CM4 ttyAMA0**, 38400, GPIO14/15. USB rejected on VBUS-parallelling and VL805
  latency grounds.
- **TIME2 → CM4 J2 pin 9 (SYNC_OUT)**, 47 Ω series at F9T end, coax, shield bonded at
  CM4 end only. chrony sees pulses. Header label misleading; driver pin name is the one
  that matters.
- **Don't hand-wire USB D+/D− header pins.** Moot now link is UART, but kept: cut USB
  cable is real 90 Ω pair with drain and is acceptable; flying leads are not.

**Still outstanding:**

**Guiding decision: keep existing configuration.** F9T already carries site-surveyed
fixed position. That is the good configuration and it stays. Goal is to *read it back and
understand it*, so it can be worked with when needed — **not** to re-run survey-in, which
would throw away settled position for a worse one. Everything below is a read unless it
says otherwise.

- **Configuration read back 2026-09-10.** `[meas]` `ubxtool` 3.25 was already installed.
  RAM and flash agree; BBR is empty. TMODE is fixed LLA (`MODE=2`, `POS_TYPE=1`) at
  60.179644722°, 24.958692222°, 19.00 m — not survey-in or continuous solving.
- **TP2 read back 2026-09-10.** `[meas]` Enabled, 1 Hz and 50% duty. Both unlocked and
  locked frequency fields are 1 Hz, but `USE_LOCKED_TP2=0`, so the unlocked waveform is
  deliberately used in both states; the zero locked-length/duty fields cannot stop the
  pulse after fix. RAM and flash agree. No write made.
- ~~**Antenna cable delay read back 2026-09-10:** `CFG-TP-ANT_CABLEDELAY=5` ns.~~
  **Corrected to 40 ns, 2026-09-11.** `[meas]` The run is 8 m (5 m + 3 m, RG-174/RG-58
  class, solid PE, VF 0.66):

      8.0 m / (0.660 × 299792458 m/s) = 40.4 ns

  So the stored 5 ns under-compensated by **35 ns**, and the time pulse was that much
  late against UTC. This was the largest single error in the whole chain — larger than
  chrony's PPS standard deviation, and more than ten times the receiver's own reported
  `tAcc`. Being systematic rather than random, no amount of averaging touched it.

  Refinable by a couple of ns if the cable turns out to be PTFE (VF 0.695 → 38 ns) or
  foam PE (VF 0.83 → 32 ns). Does not include antenna LNA group delay, unspecified here
  and typically 10–30 ns; fold that in if it is ever measured. `[?]`
- **Antenna run changes invalidate this.** Re-measure and re-enter if the run is re-made
  (≈5 ns/m for RG-58 at 0.66 VF). The antenna was moved on 2026-09-11 but the cable was
  not re-made.
- **gpsd device corrected.** `[built]` 2026-09-10: `/etc/default/gpsd` names
  `/dev/ttyAMA0`; gpsd is active and identifies the ZED-F9T at 38400. Antenna is detached
  until the rack move, so no valid fix or UTC is expected now. chrony's NMEA/SHM source
  is still commented out; restore and validate it after the antenna is reattached so the
  seconds label disambiguates which second a PPS edge belongs to.
- **Stale ser2net path disabled.** `[built]` 2026-09-10: ser2net still listened on TCP
  2000 with `/dev/ttyAMA0` as connector from the former USB-forwarding arrangement. It
  did not hold the UART until a client connected, but would then contend with gpsd.
  Service stopped and disabled; gpsd is now the sole ttyAMA0 owner.
- Antenna shield bonding point, and surge protection if roof-mounted. DC-*passing*
  arrestor — plain DC block kills active antenna bias. `[?]`

**Header pinout** (from breakout drawing): +5V, GND, RX(2)/TX(2), TIME(2)/TIME(1),
EXTINT, READY, SCL/SPI_CLK, SDA/SPI_CS, USB D−/D+, RX/SPI_MOSI, TX/SPI_MISO. SEL pad
selects UART+I²C vs SPI. TIME(1) = TIMEPULSE1 = 1 PPS by default. +5V and GND pins are
diagonally adjacent rather than in a row — verify against physical board before
committing.

**Consolidation worth considering:** if squarer/divider board, Si5351 and Teensy all end
up in this same enclosure, EXTINT becomes internal too and only things crossing the panel
are 10 MHz in, 5 V in, 54 MHz out, USB/UART to CM4. Every fast edge stays on one ground
plane, which makes most of §5 stop mattering.

---

## 7. Open items

Grouped by what access each needs — software, DMM, soldering iron, or rack with Orion
attached. See sequencing note below for why that ordering changed.

### Closed

- ~~**Measure AR-40A open-circuit output and source impedance.**~~ 2026-09-07: 4.44 Vpp
  open, 46.2 Ω source, +11.25 dBm available.
- ~~**Fit the pad.**~~ 2026-09-07: DIY 7.01 dB pi, verified to 1% of prediction. Rebuilt
  2026-09-09 with same 150/51/150 values; confirmed indirectly by CLKIN measuring
  2.1 Vpp clamped from expected 2.49 Vpp, only reachable if pad delivers 1.245 Vpp into
  the DA.
- ~~**Resolve AR-40A BIT polarity contradiction.**~~ Settled by existing lock interlock —
  see §3.
- ~~**Check what Si5351 breakout does at its SMA input.**~~ 2026-09-09: nothing. CLKIN
  sits at 0 V DC and clamps on negative half (§8), so no bias network on breakout.
- ~~**Verify the chain end to end.**~~ 2026-09-09: AR-40A → pad → DA R in → DA R out →
  CLKIN unterminated. PLLA locks, CLK4 reads 54.000 MHz, CM4 boots.
- ~~**Verify supply rails.**~~ 2026-09-09: all four within 23 mV, CM4 at 4.917 V booted
  (§5).

- ~~**Flash and verify gating firmware.**~~ 2026-09-09: running. CLK4 gated on LOS_CLKIN
  / LOL_A; verified against real AR-40A power cycle — reference lost, sticky 0xF0
  confirming drop was genuine, CLK4 disabled, then re-enabled ~1 s after signal returned.
  BIT proven both directions (LOCKED → UNLOCKED → LOCKED). Warm-up behaviour confirmed:
  with CLKIN present but BIT unlocked, CLK4 stays enabled, so CM4 not held off during
  rubidium pull-in. Panel LEDs wired and working.

  Two bugs found and fixed along the way: failed I2C read returned 0xFF and was
  indistinguishable from every fault bit set, so single bus glitches killed CLK4 for a
  second at a time — now a third state that holds the output; and single-poll blip could
  disable, now debounced at two consecutive bad reads.

- ~~**Confirm F9T breakout I/O level.**~~ 2026-09-09: 3.3 V, confirmed by UART decoding
  cleanly on GPIO15 (§3).
- ~~**Decide F9T data link.**~~ 2026-09-09: UART1 on ttyAMA0 at 38400. USB rejected —
  breakout ties header +5V to USB VBUS, and CM4 USB sits behind VL805 over PCIe.
- ~~**Wire F9T PPS.**~~ 2026-09-09: TIME2 → J2 pin 9 (SYNC_OUT), 47 Ω, shield at CM4 end
  only. chrony sees pulses. Settled pin-8-vs-9 question and 1.8 V-vs-3.3 V documentation
  conflict at same time (§3).
- ~~**Cut Teensy VUSB pad.**~~ 2026-09-09. USB and external 5 V can now coexist.
- ~~**Teensy → CM4 status link.**~~ 2026-09-09: Serial1 → ttyAMA5 on GPIO12/13, 115200.
  Firmware reports to USB and Serial1 together (§3).
- ~~**Image the eMMC.**~~ 2026-09-09, via rpiboot mass-storage-gadget64. Motivated by
  discovering kernel carries non-standard BCM54210PE timestamping driver (§3). **Image
  verified readable 2026-09-10.** Unopened image is hypothesis, not backup; this one is
  now actually a backup of the irreproducible kernel.
- ~~**Drop `dtoverlay=miniuart-bt`.**~~ 2026-09-10. `disable-bt` kept (§3).
- ~~**I2C data integrity — fix flashed and premise verified.**~~ 2026-09-10. Bus to
  100 kHz; reg 20 read back at init and **verified 0x4F**, closing CLK4 drive-strength
  question for good; fourth gating state for readings rejected by reg0/reg1 sticky
  cross-check, counted as `dataErr` because `i2cErr` only counts failed transactions and
  was under-reporting.

  **`FAULT_MASK` is 0x30 — LOL_A and LOS_CLKIN only.** Measured: sticky reads 0xC0 on
  every steady-state poll, i.e. SYS_INIT_STKY and LOL_B_STKY both re-latch immediately
  after each clear, so neither can ever contradict a live bit. Glitched reg0=0xC1 was
  observed sailing through earlier 0xB0 mask with `dataErr=0` for exactly this reason.
  `[meas]`

  **Premise confirmed, not assumed:** first poll after init read `sticky=0xE0` —
  LOL_A_STKY latched while PLLA was acquiring — and every poll since reads 0xC0. So
  LOL_A_STKY latches on the event and stays clear once condition is gone, which is what
  makes the cross-check meaningful rather than vacuous. `[meas]`

  Consequently **SYS_INIT reported but no longer gated on**: its sticky cannot police it,
  so single flipped bit 7 would otherwise be two polls from cutting CM4 clock. Startup
  still covered by SYS_INIT wait in `si5351Init()`, and genuine device reset shows on
  LOL_A too.

  Gating sequence confirmed on this build: `CLK4=off` on first report, enabled on fourth
  clean poll, steady thereafter.

### Sequencing — what the constraint actually is

**Rack move happened 2026-09-10.** Rig is in the rack on its own supplies. The connector
build (item 7) did not happen before it, so the DA/Orion loaded measurement (item 8) now
needs the scope carried to the rack rather than the box carried to the bench. Everything
else that the move gated is either done or unaffected.

Original reasoning, kept because it still explains the ordering: rack move is last moment
box and scope are on same table, and after it Orion is available
as load. Used to imply large "do it before the move" batch. No longer does:

- CLK4/XIN amplitude question deferred until second CM4 exists (§8), so not a bench item
  at all.
- EXTINT already on BNC (§3), so no panel work outstanding.
- Teensy USB reachable from rack, and its status text also reaches CM4 on ttyAMA5, so
  **firmware iteration does not require pulling the box.**
- AR-40A chassis-to-earth check is property of the unit, not of the bench.

Left before the move: building the connectors. The DMM check can happen whenever the
AR-40A is reachable. Firmware flashing/soak and live GNSS validation wait until after
the rack move.

### Open — after the rack move, software

1. ~~**Flash and exercise OE-readback firmware.**~~ Done 2026-09-10. `setClk4()`
   verifies register 3, reports the actual OE byte, leaves state unknown on failed
   write/readback and counts `oeErr`. Exercised on hardware: healthy reports read
   `CLK4=on OE=0xEF oeErr=0` across an AR-40A power-cycle and an unlocked→locked BIT
   transition.
2. **`dataErr` soak — inconclusive, errors persist at low rate.** `[meas]` Soaked in the
   rack on the rig's own supplies with the laptop disconnected — the exact conditions the
   earlier bench test could not provide. Result: `dataErr` still rises slowly. Not
   alarming (rejections are safe by construction — CLK4 is held, `i2cErr` and `revidErr`
   stay zero), but not zero either.

   So the decision rule from the bench has been overtaken. It read "anything above zero
   means fit external 2.2k–4.7k to 3V3 on SDA/SCL" — but those are already fitted: 4.9 kΩ
   on each of SDA and SCL to 3.3 V, ≈3.6 kΩ effective in parallel with the internal
   pull-up, and the errors survived both that and the move to clean supplies. Neither bus
   speed nor pull-up strength nor the laptop's ground was the whole story.

   **What is still missing is the rate, not the fact.** "Casually rising" is not a
   number; the next useful datum is errors per hour from a known-zero reset, which turns
   this from a yes/no into something that can be compared against a change. Next
   candidate change remains a second 4.9 kΩ in parallel per line (≈2.1 kΩ effective),
   but measure the current rate first so the comparison means something. `[?]`
3. ~~**Validate GNSS with the antenna reattached.**~~ **Done 2026-09-10 — rig is
   stratum 1.** `[meas]` See §9 for the whole diagnosis; summary:

   - **Antenna siting is fine.** 3D fix, 12 SVs used, PDOP 1.73, best C/N₀ 43 dB-Hz,
     reported position agrees with the surveyed TMODE fixed position. It had not been
     sited deliberately, so this was not a given.
   - **chrony's refclocks were never miscommented or misconfigured.** Both
     `refclock PHC /dev/ptp0:extpps ... refid PPS2 lock NMEA prefer` and
     `refclock SHM 0 ... refid NMEA offset 0.028 noselect` were live and correct, and
     both read `Reach 0`. The fault was upstream in gpsd.
   - **After restarting gpsd:** `PPS2` selected, **Stratum 1**, last offset **−11 ns**,
     estimated error **±19 µs**, `Reach 377` on both refclocks.

   Note the antenna run may have changed length at the move — if so the stored
   `CFG-TP-ANT_CABLEDELAY=5` ns no longer matches the cable (§6b). Still `[?]`.

### Closed — the DMM session, 2026-09-10

6. ~~**Is AR-40A chassis at mains PE?**~~ **Yes — continuity confirmed** `[meas]`. The
   10 MHz chain therefore has **two** earth injections, AR-40A and Extron, with the coax
   shield closing the loop between them (§5).
9. ~~**AC volts between grounds.**~~ CM4 ↔ Extron chassis reads **<3 mV AC** `[meas]` —
   the last missing reading. Both earth injections are real and the loop is closed, but
   it develops single-digit millivolts, so there is no loop current worth chasing and
   nothing to do about it. Do not lift either earth. Earth-path question closed (§5).

### Open — build the connectors, now at the rack

7. **DIY a 75 Ω terminator, a 50 Ω terminator and a BNC tee** from chassis connectors
   already on hand. **Skipped before the move due to circumstances**, so it is now the
   single thing gating item 8, and item 8 needs the scope brought to the rack. See construction note below. Without known-value terminator of his
   own, item 8 result is ambiguous between "DA sags into a real load" and "Orion doesn't
   terminate at 75 Ω" — terminator is what makes that measurement mean anything, so not a
   stopgap for a bought part.

### Open — one measurement session at the rack, scope carried to it

8. **DA output into real 75 Ω load, and Orion actual termination — one procedure, three
   readings** at Orion input, same setup, same probe:
   1. Cable open → **V_open**, DA unloaded output.
   2. Known 75 Ω terminator on free leg of tee → if reads ≈ V_open/2, DA build-out really
      is 75 Ω *and* it drives a real load without sagging. Closes item-8 question on its
      own.
   3. Orion connected → with source impedance now confirmed, Orion input resistance is
      75 × V_orion / (V_open − V_orion). ~1.26 V against ~2.5 V open means it terminates
      at 75 Ω.

   Measured clean unloaded already (§3), ruling out voltage clipping and slew limiting;
   untested is current drive. **No longer gates the board** — jumper-selectable 75/50 Ω
   input termination covers either outcome (§6).
9. ~~**AC volts between grounds.**~~ Closed 2026-09-10, see above — <3 mV CM4↔Extron.
   The refinement never taken, and no longer worth taking on this result: readings with
   the 10 MHz coax disconnected as well as connected, whose difference would size the
   earth-path effect. At 3 mV there is nothing to size.
10. **Antenna — now attached, inspection still outstanding.** Confirm where coax shield
    is bonded; if roof-mounted, gas-discharge arrestor at entry point. GNSS DC block / galvanic isolator removes whole class of
    ground problem, provided it passes active antenna bias. Also settles whether antenna
    run — and therefore stored CFG-TP5 delay — changes at the move (§6b).

### Deferred — waiting on hardware that isn't here

11. **CLK4 amplitude / CM4 XIN receiver** (§8). Needs second identical CM4 module to
    experiment on, and real output is proper circuit at CM4 end rather than resolved probe
    reading. Not chased on running rig. Feeds §6 scope question.
12. **The squarer/divider board itself.** Blocked only on §6 scope question (enclosure
    consolidation, feed point, 54 MHz drive), not on any measurement.
13. **Boot-console bridge.** GPIO14/15 are occupied by the F9T. A Teensy spare-UART to
    USB bridge remains a candidate, but is explicitly deferred; it does not block the
    rack move or timing work.

### Open — receiver configuration, once the antenna is finally sited

14. ~~**Only GPS and Galileo E1 are actually live.**~~ **Done 2026-09-11.** `[meas]`
    `GAL_E5B`, `BDS_B1` and `BDS_B2` enabled, making Galileo dual-frequency to match GPS
    and adding BeiDou as a third system. The seven RTCM3 base-station message types were
    switched off on UART1 at the same time — nothing consumed them and they were a large
    part of the ~250 ms serial cycle latency.

    Measured before → after: satellites used **11 → 23**, PDOP **3.74 → 1.65**, TDOP
    **3.02 → 1.05**, receiver `tAcc` **3 ns → 1 ns**. The antenna was repositioned in the
    same window, so the C/N₀ improvement (43 → 48 dB-Hz peak) cannot be attributed to the
    constellation change alone.

    GLONASS, QZSS and SBAS remain deliberately off; reasoning is recorded in
    `config/f9t-timing-changes.txt` so it does not get "fixed" by accident.

    Original finding, kept because the trap is worth remembering: the per-constellation master
    switches are misleading: `CFG-SIGNAL-{SBAS,BDS,QZSS,GLO}_ENA` all read 1, but every
    signal underneath them reads 0, so they contribute nothing. What is genuinely
    enabled is `GPS_L1CA`, `GPS_L2C`, `GAL_E1` — confirmed both from config and from the
    sky view, which only ever shows gnssId 0 and 2.

    So **GPS is dual-frequency and Galileo is not.** For timing that ordering matters
    more than constellation count: L1+L2C gives an ionosphere-free combination, which is
    the dominant residual error at this level. **Enabling `GAL_E5B` is the single
    highest-value change** — it buys Galileo the same treatment. Adding GLONASS or BeiDou
    afterwards helps DOP but not accuracy, and costs UART bandwidth (§9): `UBX-NAV-SAT`
    grows with satellite count. Raising UART1 to 115200 remains open and is the next
    bandwidth step if more signals are ever added. `[plan]`

15. **Survey the antenna position — running since 2026-09-11.** `[built]` See §10 for
    the DGNSS attempt and why the survey is running standalone. Started with
    `f9t-survey start 10`; `f9t-survey status` reports progress, `f9t-survey finish`
    writes the mean to Flash, sets `FIXED_POS_ACC` to the achieved standard error, clears
    the stale ECEF fields and restores fixed mode.

    **The antenna was moved on 2026-09-11**, so the stored position is not merely
    imprecise, it is stale. Early rover fixes put the antenna several metres from the
    stored coordinates, i.e. of order 19 ns of bias.

    TMODE is dropped in the RAM layer only for the duration, so a power cut during the
    survey restores fixed mode rather than leaving the rig a rover. Timing runs degraded
    while it is in progress: the receiver is a rover and the PPS carries position-solution
    noise.

    Original reasoning:

    In TMODE
    fixed the receiver treats the configured position as truth, so error in that position
    maps almost directly into a *constant time bias* — roughly **3.3 ns per metre**.

    Current stored value is `CFG-TMODE-LAT 601796447`, `LON 249586922`,
    `HEIGHT 1900` (cm) plus the HP fields, i.e. 60.1796447°, 24.9586922°, 19.00 m. The
    horizontal figures look like a survey-in product; **the height is exactly 19.00 m**,
    which is an entered round number, not something a survey returned. If it is wrong by
    a couple of metres that is ~7 ns of bias — more than twice the receiver's own
    reported `tAcc` of 3 ns, and a systematic error, so unlike jitter it never averages
    out.

    The fix is a one-off, not a subscription: take RTK corrections (Maanmittauslaitos /
    FinnRef NTRIP), run the receiver as a rover until it reports an RTK *fixed* solution,
    average, write the result back into `CFG-TMODE-*` and turn the corrections off again.
    Continuous RTK buys nothing afterwards — with the position known and GPS already
    dual-frequency, there is no remaining error for the corrections to remove.

    Sequencing: this is worth doing **only after the antenna is in its final position**
    (§7 item 10), since re-siting invalidates both the survey and the cable delay.

    Two practical snags to expect. gpsd owns `/dev/ttyAMA0` and runs with `--passive`, so
    it will not write to the receiver; injecting RTCM means letting gpsd carry an NTRIP
    source or giving up the port for the duration. And the receiver is presently *emitting*
    RTCM3 itself — a side effect of TMODE fixed making it an RTK base — which is pure
    UART load here and could be turned off regardless.

### Connector construction note

At 10 MHz wavelength in coax is ~20 m, so few centimetres of unmatched junction is
electrically invisible and home-made parts are not a compromise. Small axial metal-film
resistor carries few nH of parasitic inductance — j0.3 Ω against 75 Ω at 10 MHz. An 0805
soldered straight across a BNC is better still.

What matters is **value, not RF construction**: measure resistor on DMM and use measured
figure in the arithmetic. 75.0 Ω exists in E96, but measured 74.3 Ω used knowingly beats
nominal 75 Ω used blindly.

Build terminators as **male plugs** rather than feedthrough barrels where there is a
choice — plug goes on free leg of tee, which is where it is wanted, and doubles as load
elsewhere. Make the 50 Ω one at same time for bench-instrument leg. Keep leads short and
resistor body inside the shell.

Still worth buying eventually, but no longer blocking anything: proper 75 Ω feedthrough
(SDS1104X-E is 1 MΩ fixed, no switchable 50 Ω) and SMA attenuator kit (1/2/3/6/10 dB) for
general bench work.

---

## 8. Things known to be out of spec but working

Kept here so they aren't "fixed" without thought.

- **54 MHz into CM4 XIN at full 3.3 V CMOS swing** through 10 Ω. Crystal inputs generally
  want ~1 Vpp AC-coupled. Stable for years.

  Measured 2026-09-09: **3.4 Vpp** at CLK4 `[meas]` — full rail plus overshoot from
  unterminated line. Earlier reading of 1.22 Vpp / 394 mV rms was taken at different point
  or under different loading; initially explained as scope 100 MHz bandwidth rounding a
  54 MHz square, but that cannot account for both figures on same instrument, so that
  explanation is withdrawn. Difference is load, not filtering. Worth resolving which probe
  point gave which, since genuinely loaded 1.22 Vpp would imply XIN presents far lower
  impedance than a crystal input nominally does — likely internal Pierce amplifier
  running. `[?]`

  **Deferred, deliberately.** Not chased on this rig. Characterising XIN properly means
  loading and probing an input the whole system depends on, and the useful output is a
  designed receiver circuit at CM4 end rather than a settled scope reading. Both want a
  **second identical CM4 module** to experiment on. Until one turns up this stays as it is
  — worked for years — and board is laid out to anticipate it (§6).

  For PPS timestamping accuracy this path is close to irrelevant anyway — few ps of jitter
  on CPU clock disappears under interrupt latency. Matters for boot stability and system
  clock frequency accuracy at long tau, not for edge capture.
- ~~**AR-40A into Extron at 2.75 Vpp** vs 1.5 Vpp max.~~ **Resolved 2026-09-07** by the
  7.01 dB pad; DA now sees 1.245 Vpp. Kept as history — the years spent overdriven are the
  reason item 8 in §7 is still worth measuring.
- **Zero-centred sine into Si5351 CLKIN — CONFIRMED BY MEASUREMENT 2026-09-09.** `[meas]`
  Fed 2.49 Vpp unterminated from DA output, CLKIN measures **2.1 Vpp / 776 mV rms, visibly
  asymmetric**. That is the predicted clamping: positive peak sits near +1.25 V unclamped
  while negative is held around −0.85 V by input protection diode plus source impedance,
  summing to 2.1 Vpp instead of 2.49. Crest factor 2.705 vs 2.828 for clean sine — one half
  compressed.

  So protection diode conducts every cycle and acts as the level shifter, against a −0.5 V
  absolute maximum. Functionally worked for years, and Si5351's own PLL contributes far
  more jitter than this does. Still worth fixing on new board for two reasons: sustained
  current through a protection diode is not what it is for, and threshold crossing is set
  by a diode forward characteristic, which drifts with temperature rather than sitting at
  fixed bias. This is the measured justification for the §6 input stage.

---

## 9. The dead RTC → gpsd → chrony failure chain — diagnosed 2026-09-10

Worth a section of its own because nothing in it is visible from the timing side. The
symptom is "chrony is stratum 3 off the network and both GNSS refclocks read `Reach 0`",
and every obvious suspect — antenna, receiver config, chrony config, file permissions —
was innocent.

### The chain

1. **The CM4 IO Board's RTC has no working backup cell.** `[meas]` `dmesg`:

   ```
   rtc rtc0: Power loss detected, invalid time
   rtc-pcf85063 10-0051: registered as rtc0
   rtc-pcf85063 10-0051: hctosys: unable to read the hardware clock
   ```

   It is a real **PCF85063** at I²C `10-0051`, not a pseudo-RTC — the one on the IO
   board, whose battery connector is unpopulated or flat. `/sys/class/rtc/rtc0/hctosys`
   is `0`, i.e. the system clock was *not* set from it.

2. **`fixrtc` on the kernel command line then guesses the time from the filesystem.**
   With no RTC to read, boot came up believing it was **2024-08-08 17:51** — the root
   filesystem's timestamp, roughly two years stale. `who -b` still reports that bogus
   boot time; `uptime -s` reports the corrected one.

3. **gpsd starts at that bogus time.** `gpsd.service` is `After=chronyd.service`, so it
   comes up early, long before the clock is fixed.

4. **chrony steps the clock by 65,939,623 s** (~763 days) once the MIKES servers
   answer — 13 minutes into uptime, not immediately, because the network came up late:

   ```
   Aug 08 18:03:49 aika chronyd[642]: System clock wrong by 65939623.024447 seconds
   Sep 10 22:37:32 aika chronyd[642]: System clock was stepped by 65939623.024447 seconds
   ```

5. **gpsd never ships another SHM sample.** It survives the step as a process but stops
   writing NTP0. Verified from both ends: `ntpshmmon` run as root saw **zero** samples in
   10 s, while `/proc/<pid>/maps` showed gpsd *and* chronyd both still attached to
   `SYSV4e545030`. So the plumbing was intact and simply carried nothing.

6. **`lock NMEA` propagates the starvation to the PPS.** `PPS2` is the PHC refclock and
   is locked to `NMEA` for second-numbering. NMEA delivering nothing means PPS2 cannot
   number its pulses, so *both* refclocks read `Reach 0` and chrony falls back to the
   network at stratum 3. One dead coin cell, two dead refclocks.

### Proof it is gpsd and not the config

Stopping the service and running gpsd in the foreground with the *identical* options
produced samples immediately, once per second:

```
gpsd:PROG: NTP:SHM: ntpshm_put(NTP0, -1) /dev/ttyAMA0,  1789070511.000000000 @  1789070511.242462334
gpsd:PROG: UBX: cycle end x0121 iTOW 417730000
```

Restarting the service was the whole fix. Within 40 s:

```
#* PPS2       0   4   377    10    -11ns[  -14ns] +/-   19us
#? NMEA       0   4   377     9   +215ms[ +215ms] +/-  534us
Reference ID : 50505332 (PPS2)   Stratum : 1
```

**A restart of gpsd is therefore the recovery action** any time the GNSS refclocks are
unreachable after a cold boot. It is not a fix.

### What actually fixes it

1. **Fit a backup cell to the IO board's RTC connector.** `[plan]` This removes the
   entire chain at step 1 — the clock comes up within seconds of correct, chrony never
   steps by years, and gpsd never sees the discontinuity. `rtcsync` is already in
   `chrony.conf`, so the RTC is being written every 11 minutes and will be right the
   moment it can hold charge. Cheapest possible fix for the most obscure failure here.
2. **Install `fake-hwclock`** (currently **not installed**, no `/etc/fake-hwclock.data`).
   `[plan]` Belt and braces: boot comes up at last-shutdown time rather than at the
   filesystem's date, so even a flat cell leaves a step of hours rather than years.
3. **Order gpsd after the clock is sane, or restart it once it is.** `[plan]`
   `chrony-wait.service` exists and is **disabled**. Enabling it and ordering gpsd
   `After=time-sync.target` is the tidy form, but it has a real failure mode: with the
   network down at boot, chrony has no NTP source, `chrony-wait` blocks, and gpsd — the
   only remaining time source — never starts. Prefer a small oneshot that runs
   `systemctl try-restart gpsd` after `time-sync.target`, which leaves gpsd running early
   and merely re-arms SHM after any step.

### Two smaller things this turned up

- **NMEA `offset` is wrong by ~0.2 s.** With `offset 0.028` the NMEA source still reads
  **+201 to +215 ms**, and gpsd's own SHM pair shows the cycle-ender landing ~252 ms
  after the second at 38400 baud. Harmless today: NMEA is `noselect` and exists only to
  number PPS pulses, which needs ±0.5 s, not ±0.5 ms. But it spends half the ambiguity
  budget for no reason. Correct the constant to ≈0.24 — **determine the sign
  empirically**, chrony's displayed-sample convention is easy to get backwards, and it
  costs a chronyd restart, so do it deliberately rather than while the rig is holding
  lock. `[?]`
- **38400 baud is the latency.** `[meas]` gpsd auto-detected it; nothing pins it in
  `/etc/default/gpsd` (`DEVICES="/dev/ttyAMA0"` only) and it matches the F9T's UART1.
  UART1 carries `UBX-NAV-PVT`, `UBX-NAV-SAT`, `UBX-NAV-TIMEGPS` and NMEA (gpsd tags ZDA
  as the cycle ender), plus RTCM3 that the receiver emits because TMODE is fixed. That is
  what makes the cycle land 252 ms late. 115200 would cut it to ~85 ms and widen the
  second-numbering margin. Not urgent, and it touches receiver config — the PPS path is
  unaffected either way, since PPS never goes through the UART.

### Note on the PPS path

PPS does **not** reach chrony as `/dev/pps0` — there is no such device, and gpsd logs
`ntpshm_link_activate() unable to read /dev/pps0` on every start, harmlessly. TIME2 goes
to the BCM54210PE PHY's external-timestamp input (§3, §6b), and chrony reads it as
`refclock PHC /dev/ptp0:extpps`, `bcm_phy_ptp`, `n_external_timestamps=1`. That is why the
non-standard kernel in the eMMC image is irreplaceable and why it was imaged (§7).

### Monitoring — `tools/gpsstat`

`tools/gpsstat` (installed on the CM4 at `~/bin/gpsstat`) walks the whole chain in the
order it breaks and prints one screen: chrony tracking and *per-refclock reach*, whether
gpsd is actually writing SHM(0), fix mode / satellites used / DOP / C/N₀ of the
satellites in the solution, and the hardware facts underneath — RTC power-loss, ser2net
contention, and that `/dev/ptp0` really is `bcm_phy_ptp`. Exit code 0 healthy, 1
degraded, 2 GNSS timing down, so it works from cron or a monitor as well as by hand.
`gpsstat -v` additionally polls TMODE and TP2 back from the receiver; `watch -n5 gpsstat`
gives a live view.

It exists because the failure above is invisible from the obvious command. `chronyc
tracking` during the outage showed a healthy, well-disciplined stratum-3 clock — correct
to a few hundred µs and steering nicely. Nothing was wrong except that the GNSS was
contributing nothing to it. The one number that gave it away was `Reach 0` on the two
refclocks, which `tracking` does not show at all.

It also reports accuracy from both ends: the receiver's own `UBX-NAV-TIMEGPS` `tAcc`
(3 ns on this rig), chrony's `PPS2` standard deviation (26–28 ns), and the bound chrony
actually guarantees for the system clock, root dispersion + root delay/2 (~43 µs). Note
that `RMS offset` in `chronyc tracking` is a long decay average and stays large for hours
after a step, so it is the wrong number to judge current health by — the std dev is the
honest one.

The dead RTC is reported as `NOTE` rather than `WARN`: known, on the list, and
deliberately not counted against the exit code so that a non-zero exit stays meaningful
for things that are actually news.

### Static status page — `tools/clock-dashboard`

`tools/clock-dashboard` runs `gpsstat`, parses its stable machine-relevant values, and
keeps a compact rolling JSON-lines history. It atomically generates `data.json` for the
self-contained dashboard in `dashboard/index.html`. The page plots PPS phase/scatter and
satellite usage, shows survey/fixed operating mode, the receiver time-accuracy estimate,
F9T jamming/spoof indicators, and every non-OK chain check. It visibly marks telemetry
stale if collection stops for 15 minutes. It uses no build system or external CDN, so
aika can generate it offline and publish the two static files with cron and SFTP.
Installation and the live deployment are documented in `dashboard/README.md`.

The page deliberately does not call servo offset "absolute accuracy". Constant antenna,
receiver, cable, and timestamp delays are absorbed when chrony steers the same clock it
measures; the PPP clock series described in §10 is still needed to establish absolute
UTC bias.

### Housekeeping seen in passing

- `ser2net` confirmed `inactive` after the move — the stale TCP-2000 listener that would
  have contended with gpsd for `/dev/ttyAMA0` is gone and stayed gone.
- An `ntpsec` leftover still runs an `ntploggps` cron entry. It is guarded by
  `[ ! -d /run/systemd/system ]` so it never fires on this systemd host, but it is dead
  weight and a second time daemon's packaging sitting next to chrony. `[?]`

---

## 10. DGNSS — a survey tool, not part of the running configuration

Decided 2026-09-11 after wiring the Maanmittauslaitos FinnPos DGNSS feed up and
getting corrections as far as the receiver. Recorded here because the instinct that
corrections should help a timing receiver — and should help *more* once it is a properly
surveyed base station — is reasonable, wrong, and worth not re-litigating.

### The service

`opencaster.nls.fi:2102`, mountpoint `DGNSS-MSM1`, RTCM 3.2 MSM1: 1006 station ARP,
1071/1081/1091/1121 observations for GPS/GLONASS/Galileo/BeiDou. Free, registration
required. **Code-differential, about 0.5 m at best** — not RTK. Credentials live in
`.dgnss`, gitignored, never in gpsd's config or command line.

### Why it does not belong in the steady state

**A base station does not consume corrections.** In TMODE fixed the position is held
constant by definition, so there is no position solution left for corrections to improve,
and the receiver ignores incoming RTCM. Surveying the position properly makes the rig a
better *source* of corrections for other people, not a better consumer of them.

**And if they were applied, they would corrupt the time solution.** A code correction is
formed at the reference station as *measured pseudorange − computed range*, and that
difference contains the base receiver's own clock offset, common to every satellite in
the set. For **positioning** this is harmless: the rover's clock is a nuisance parameter,
the base clock is absorbed into it, and the position comes out right. For **timing** the
clock is the answer, so applying those corrections would make the rig traceable to a
FinnRef station's clock instead of to UTC — importing an unknown offset directly into the
quantity being measured. DGNSS trades clock accuracy for position accuracy, which is
exactly the wrong direction here.

So DGNSS is only ever useful during the **survey window**, where the rig runs as a rover
and the base-clock nuisance is harmless because the rover clock is discarded anyway.

### What actually stopped it working, for the record

Corrections reached the receiver intact and were still never applied. Three measurement
points:

- Decoding the caster's raw byte stream: 1006 and 1008 present, 7 each in 75 s, plus the
  observations at 1 Hz. The service is fine.
- `UBX-RXM-RTCM` on the receiver: 1013, 1030, 1031, 1033, 1230, 1303, 1304 all arriving
  at the expected 4 per 40 s with `flags=0x0`, i.e. good CRC — but **never 1006 or 1008**.
  Exactly the two reference-station messages go missing across gpsd's relay while
  everything else passes.
- `UBX-NAV-PVT` therefore stayed at `flags=0x1`: `gnssFixOK` set, `diffSoln` clear, `hAcc`
  ~1.9 m, no differential improvement.

Without the station ARP the receiver cannot form a differential solution, so the
observations are useless on their own. `[meas]`

Three gpsd limitations were found getting that far, none of them documented anywhere
obvious:

1. **gpsd's own `ntrip://` refuses the stream.** Its format table has `RTCM 3.2` with a
   space and `RTCM32`, but not `RTCM3.2` — which is exactly what the caster advertises.
   Authentication and sourcetable parsing both succeed; it then rejects a stream it is
   perfectly capable of handling.
2. **A plain `tcp://` device is never relayed.** gpsd gates the RTCM relay on the device's
   *service type*, not on the packet type, so the bytes arrive and stop there.
3. **`dgpsip://` drops the port** from the URL and expects a handshake of its own.

`tools/ntrip-relay` works around the first by pulling from the real caster and re-serving
the identical bytes as a minimal local NTRIP caster advertising the spelling gpsd accepts.
That gets gpsd relaying — and is where the 1006/1008 loss shows up. Its `--serial` option
writes RTCM straight to the receiver instead, which is the right way to bypass the relay
entirely (gpsd runs `--passive` and never writes to the port, so there is no contention
over writes); it does not help here because **gpsd holds the port exclusively (`TIOCEXCL`)
and the open fails with `EBUSY`**. Making that work would mean restructuring so the relay
owns the port and gpsd reads a pty behind it — a new failure point in the data path, for a
service capped at 0.5 m. Not worth it.

### Verifying the time bias — the part nothing on the rig can do

Worth stating plainly, because `chronyc` looks like it ought to answer it and does not.

chrony measures the PPS against the system clock **and steers the system clock to the
PPS**. A constant offset in when the pulse occurs is absorbed entirely by the servo, so
`sourcestats` reports `Offset -0ns` whether the pulse is on time, 35 ns late, or 35 µs
late. That column reports servo convergence, not accuracy. `Std Dev` (22 ns here) is
real information — it is jitter, and it is where geometry improvements show — but it
says nothing about bias either.

The 35 ns cable-delay correction therefore **cannot be observed on this rig**, only
calculated. It appeared once, as a step at the instant of the write, and even that is
unrecoverable: the constellation change restarted the GNSS subsystem in the same
operation and threw a disturbance five orders of magnitude larger (−6202 µs offset,
20 ms std dev). The NTP servers cannot arbitrate either — MIKES shows ~14 ms root delay
and ~1 ms sample noise, roughly 10⁵ too coarse.

The same blindness covers the **antenna LNA group delay** (typically 10–30 ns,
unspecified for this antenna) and the receiver's internal delay. Both sit uncompensated
inside `CFG-TP-ANT_CABLEDELAY` alongside the 40 ns cable term.

A PPP clock solution measures all of it at once, against a global timescale, with
nothing borrowing another receiver's clock. That is the only route from *calculated* to
*measured* below about 10 ns, and it needs no extra hardware — just the RAWX already
being logged. `[plan]`

### What is worth it instead

`UBX-RXM-RAWX` is logged throughout the survey. That feeds a **PPP** solution — RINEX to
a free service such as NRCan CSRS-PPP — good to centimetres, so ~0.1 ns, against the
~1.7 ns floor that 0.5 m code corrections would have given.

PPP is the right endgame for precisely the reason DGNSS is the wrong one: it uses precise
satellite orbit and clock products referenced to a global timescale, applied after the
fact, so nothing borrows another receiver's clock. `[plan]`

### Tooling added

| Tool | Purpose |
|---|---|
| `tools/gpsstat` | one-screen health of the whole chain, exit-coded for cron (§9) |
| `tools/ubx-dump-config` | full CFG dump, both layers, paged — the backup in `config/` |
| `tools/ubx-apply-config` | apply a key/value file, verifying every write by readback |
| `tools/f9t-survey` | run the position survey and freeze the result back into TMODE |
| `tools/ntrip-relay` | NTRIP client and local re-caster; unused in steady state |
| `tools/f9t-ppp` | RAWX → RINEX via `convbin`, for submission to a PPP service |
| `tools/clock-dashboard` | archive `gpsstat` snapshots and generate static dashboard data |

Two traps these encode, both of which cost real time:

- **`ubxtool` must name its gpsd device** whenever more than one is attached, or gpsd
  answers `No path specified in DEVICE, but multiple devices are attached` and every
  readback silently returns empty. This first appeared as a survey that *reported*
  disabling TMODE while changing nothing. The readback caught it; without it, hours of
  data would have been logged in the wrong mode.
- **`UBX-CFG-VALGET` returns at most 64 items**, so any dump of a large group must page
  with the `position` field. The first backup silently truncated `CFG-MSGOUT` to 64 of its
  561 keys — useless for exactly the group most likely to need restoring.
