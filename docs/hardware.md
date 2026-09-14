# Hardware

Reference for each component as built. *Spec* means a datasheet or manual value. All
other figures were measured on this rig, and dates are given where they matter. Open
work is in [plan.md](../plan.md).

## Power

- **MeanWell RS-15-5** (5 V, 15 W) is star-wired from its terminals with three separate
  runs: CM4, Si5351 + Teensy, and ZED-F9T. The Teensy chains off the Si5351 board because
  they are physically attached. The output floats (FG to DC output reads open), so the
  only coupling to earth is the Y-capacitor.
- Rails measured at the far end of each run (2026-09-09): CM4 4.936 V idle / 4.917 V
  under load, Teensy 4.932 V, Si5351 4.937 V, F9T 4.955 V. CM4 headroom on a 15 W
  supply has not been measured.
- Cable is audio multicore. Each inner pair carries +5 V on hot and the return on cold.
  The shield is bonded at the PSU end only and never carries return current.
- **MeanWell LRS-75-15** (15 V) powers the AR-40A. Its −V is **not** tied to the 5 V
  supply ([grounding](grounding.md)).
- Everything powers up directly. The old relay interlock, where AR-40A BIT switched the
  5 V feed, has been removed. CLK4 gating in firmware now does that job.

## AccuBeat AR-40A rubidium

| Parameter | Value |
|---|---|
| Output | 10 MHz sine. Spec +12±2 dBm into 50 Ω. Measured 4.44 Vpp open circuit from a 46.2 Ω source, +11.25 dBm available |
| Supply | 15 V. Measured 1.3 A warm-up, settling to 0.600 A in under 5 min |
| Warm-up (spec) | 5 min to lock, 7.5 min to 5×10⁻¹⁰. The output is present from power-on |
| Aging (spec) | <1×10⁻⁹ first year, <5×10⁻¹⁰/yr after |
| ADEV (spec) | <3×10⁻¹¹ @ 1 s, <3×10⁻¹² @ 100 s |
| Temperature (spec) | ±2×10⁻¹⁰ over −5…+50 °C |
| Trim (spec) | trimmer under the calibration sticker, 1 turn ≈ 5×10⁻¹⁰, 10 turns total |
| BIT | DB9 pin 3, open collector. It sinks to ground when locked |

- **Frequency offset vs GNSS: −2.55×10⁻⁹ ±5×10⁻¹¹** since the 2026-09-11 power cycle,
  consistent with years of normal aging. chrony corrects it in the system clock. Trimming
  is discussed in plan.md Step 3b.
- The manual contradicts itself on BIT polarity. §2.1.2 is right (locked = shorted to
  ground) and §3.3 has the labels swapped. Years of the relay interlock confirmed this.
  The sink current is not specified.
- DC resistance at the SMA reads ~84 Ω. That shows a DC-coupled resistive output. It is
  not the RF source impedance, which was measured under load.
- The unit draws 9 W steady. The manual asks for a heatsink of ≤1 °C/W, and heat feeds
  directly into the temperature coefficient.

## Pad and levels

A DIY 50 Ω pi pad sits at the AR-40A SMA: 150 / 51 / 150 Ω (measured 150.000 / 50.975 /
149.337 Ω), giving **7.01 dB**. Into 47 Ω it measured within 1% of prediction
(2026-09-07). After a rebuild on 2026-09-09, port DC resistance read 85.64 / 85.57 Ω
against 85.75 Ω predicted. Without the pad the DA received 2.75 Vpp against its 1.5 Vpp
maximum, and it ran that way for years.

| Point | Level |
|---|---|
| DA input | 1.245 Vpp, 17% under the 1.5 Vpp maximum |
| DA out → Orion (75 Ω) | 1.245 Vpp, 25% over 1 Vpp nominal, comparator input |
| DA out → bench (50 Ω) | 0.996 Vpp ≈ +3.9 dBm |
| DA out → Si5351 CLKIN (unterminated) | 2.49 Vpp nominal, 2.1 Vpp measured (clamped, see below) |
| Load seen by the AR-40A | 60.4 Ω, ~20 dB return loss |

## Extron DA (RGB/YUV distribution amplifier)

- Spec: 75 Ω input, 0.3–1.5 Vpp, 350 MHz bandwidth. Outputs have a 75 Ω build-out and
  reach unity gain only into a 75 Ω termination.
- Settings: **R plane, Gain and Peak DIP off, AC coupling on.** Keep gain at unity.
- R output unloaded measured 2.52 Vpp against 2.49 Vpp predicted, with a clean crest
  factor, so there is no clipping or slew limiting. Drive into a real 75 Ω load has not
  been tested ([plan.md](../plan.md)).
- Use only the R, G and B planes. The H/V sync inputs are 510 Ω TTL squarers rated
  15–180 kHz. On a DA6 YUV A, avoid the digital audio BNC. G and B remain spare 1→6
  buses.

## Antelope Orion 32 HD — 10M input

**In use**, fed from a DA output. Spec: 75 Ω, 1 Vpp nominal sine, self-biasing
comparator front end. The actual termination has not been confirmed. Every Orion clock mode disciplines the same internal
OCXO. The 10M input multiplies by ~×2.26 where word clock multiplies by ×512, so it adds
far less phase noise and is the right input.

## Si5351C-B breakout

| Parameter | Value |
|---|---|
| Config | PLLA from CLKIN: 10 MHz × 86.4 = 864 MHz. MS4 ÷16 → **54 MHz on CLK4** |
| CLK4 drive | 8 mA (reg 20 = `0x4F`, verified at init), 10 Ω series, coax to CM4 grounded both ends |
| CLKIN thresholds (spec) | V_IL ≤ 0.99 V, V_IH ≥ 2.31 V at 3.3 V |
| CLKIN abs max (spec) | −0.5 to +3.8 V. No internal bias |
| I2C | address 0x60 |

- **The breakout has no CLKIN bias network.** A zero-centred sine clamps on its negative
  half: 2.1 Vpp and visibly asymmetric, with the protection diode conducting on every
  cycle against the −0.5 V absolute maximum. It has worked for years. The planned
  [timing board](timing-board.md) input stage is the proper fix.
- With no reference, the loop opens, the VCO parks near 186 MHz, and CLK4 puts out a
  steady **~11.6 MHz**. The CM4 cannot boot on that. CLK4 gating exists because of this.
- Never feed 10 MHz into XA, which accepts 25–27 MHz only.
- The onboard regulator's part number and PSRR are unknown.

## Teensy 4.0 and `5351.ino`

- **I2C** on pins 18/19 at 100 kHz, with 4.9 kΩ external pull-ups to 3.3 V (≈3.6 kΩ
  effective).
- **CLK4 gating.** The Teensy polls every 250 ms. CLK4 is enabled after 4 clean polls
  with `LOS_CLKIN` and `LOL_A` clear (`FAULT_MASK 0x30`) and disabled after 2 bad polls.
  A failed I2C transaction or a rejected reading holds the current output state. A reading
  is rejected when it contradicts the sticky register or the REVID. If init has not
  succeeded, CLK4 is forced off. `SYS_INIT` is reported but does not gate. After each
  change the OE register is read back (`OE=0xEF` means on).
- **BIT is indication only.** CLK4 follows the Si5351, not the rubidium lock, so the CM4
  boots during the AR-40A's 5-minute warm-up and chrony re-converges afterwards.
- **Counters:** `i2cErr`, `dataErr`, `revidErr`, `oeErr`. `dataErr` rises slowly in
  steady state. Those rejections are safe, but the rate has not been quantified.
- **Status link:** Serial1 at 115200 to CM4 `/dev/ttyAMA5` (`dtoverlay=uart5`). Teensy
  pin 1 → header pin 33 (GPIO13), pin 0 ← header pin 32 (GPIO12), GND to header pin 34.
  It carries the same text as USB every ~2 s and on each BIT change. RX is not used yet.
  This UART shares IRQ 35 with the F9T's UART, so keep the message rate low.
- **BIT input:** pin 2, `INPUT_PULLUP`, 1 kΩ in series and 100 nF to ground. Teensy pins
  are **not 5 V tolerant**. The line is safe only because nothing else is connected to
  it, so the old relay coil must never return.
- **Panel LEDs** (~1.5 mA through 1 kΩ):

  | LED | Meaning |
  |---|---|
  | yellow, pin 3 — REF | solid: CLKIN present and PLLA locked. Blinking: settling. Off: no signal |
  | red, pin 4 — LOCK | AR-40A reports locked |
  | onboard, pin 13 | heartbeat |

  REF off means the signal path is broken (AR-40A, pad, DA or cable). REF on with LOCK
  off means the rig is running on the rubidium's free OCXO. The LEDs are wired over a
  2-conductor shielded strand whose shield is the common cathode return. That shield is
  not bonded to the chassis.
- The VUSB pad is cut, so USB and the external 5 V can coexist.

## Raspberry Pi CM4 on the CM4 IO Board

- **Clock.** The BCM2711 crystal is removed and CLK4 drives XIN through 10 Ω. XIN sees
  3.4 Vpp, full CMOS swing plus overshoot. Crystal inputs normally want ~1 Vpp
  AC-coupled, so this is out of spec on paper, but it has been stable for years. An
  earlier 1.22 Vpp reading at an unrecorded probe point is still unexplained.
  Characterisation waits for a spare CM4.
- **Power.** 5 V enters J20 pin 4 with GND on pin 3 (checked for continuity). **L5 is
  removed**, which stops the onboard 5 V and 3.3 V supplies from starting. There is no
  12 V rail, so the PCIe slot and fan header are unpowered. The 40-pin header's 5 V pins
  are on the same net, so they do not bypass L5.
- **J2 is the 14-pin configuration header:** 1 GND, 2 nRPIBOOT, 3 GND, 4 EEPROM_nWP,
  5 AIP0, 6 AIP1, 7 GND, 8 SYNC_IN, 9 SYNC_OUT, 10 GND, 11 TVDAC, 12 GND, 13 RUN_PG,
  14 GLOBAL_EN. Jumper pins 1–2 only while imaging the eMMC.
- **The PPS goes to pin 9 (`SYNC_OUT`).** The label describes the PHY's role in a 1588
  network. The driver exposes one PTP pin, `/sys/class/ptp/ptp0/pins/SYNC_OUT` = `1 0`
  (EXTTS, channel 0). Pin 8 is reportedly miswired on the CM4IO.
- **PHY:** BCM54210PE with hardware TX/RX timestamping. PHC `/dev/ptp0` is `bcm_phy_ptp`.
- **RTC:** PCF85063 at I²C `10-0051` (`dtoverlay=i2c-rtc,pcf85063a`). It had no backup
  cell ([troubleshooting](troubleshooting.md#dead-rtc--gpsd--chrony)).
- **`config.txt` settings that matter:** `dtoverlay=disable-bt` (puts the PL011 on
  ttyAMA0 for the F9T), `dtoverlay=uart5` (Teensy), `dtparam=eee=off`, `force_turbo=1`,
  `dtoverlay=disable-wifi`.

Software is described in [timing.md](timing.md).

## u-blox ZED-F9T breakout

- **I/O is 3.3 V.** The CM4 decodes its TX cleanly.
- **Data:** UART1 ↔ CM4 GPIO14/15. F9T TX → header pin 10, header pin 8 → F9T RX. The
  port is `/dev/ttyAMA0` at **115200** (since 2026-09-11). USB is not used for two
  reasons: the breakout ties header +5V to VBUS, so a cable would parallel the supplies,
  and CM4 USB sits behind the VL805 on PCIe. Don't hand-wire the USB D+/D− header pins.
- **1 PPS:** TIME2 → CM4 J2 pin 9 over miniature coax with 47 Ω in series at the F9T
  end. **The shield is grounded at the CM4 end only.** At the CM4 the pulse measures
  3.32 Vpp with 18 ns rise, which confirms the pin is 3.3 V even though some CM4 documents
  say 1.8 V.
- **TIME1** is kept free for TP1 quantisation-error reporting and as a scope reference.
- **EXTINT** is brought out to a BNC. It is unconnected and not isolated, reserved for
  the rubidium ÷10⁷ PPS from the planned [timing board](timing-board.md) (UBX-TIM-TM2).
- **Header:** +5V, GND, RX(2)/TX(2), TIME(2)/TIME(1), EXTINT, READY, SCL/SPI_CLK,
  SDA/SPI_CS, USB D−/D+, RX/SPI_MOSI, TX/SPI_MISO. The SEL pad chooses UART+I²C or SPI.
- **Antenna run:** 8 m (5 m + 3 m, solid PE, VF 0.66), giving a 40 ns cable delay. The
  antenna moved on 2026-09-11 but the cable stayed the same. If the run is re-made,
  recalculate the delay at ≈5 ns/m. The shield bonding point and surge protection have
  not been inspected.
- **Cable temperature:** PTFE coax (RG-178, RG-316) steps its delay by a few hundred ppm
  around 19–21 °C. PE is better through room temperature. This matters most on the
  antenna feed.

## Out of spec on purpose

Do not "fix" these without a reason:

| What | Why it stays |
|---|---|
| Full-swing CMOS 54 MHz into CM4 XIN | Stable for years. Changing it needs a spare CM4 to experiment on |
| Zero-centred sine clamped at Si5351 CLKIN | Works. Fixed properly only by the timing board's biased comparator input |
| PPS coax shield bonded at one end only | Bonding both ends stopped the F9T responding ([grounding](grounding.md)) |
| AR-40A frequency offset left untrimmed | chrony corrects it, and trimming would restart the aging record (plan.md Step 3b) |
