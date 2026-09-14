# Planned timing board

Design notes for a board to replace the Si5351 breakout's bare CLKIN input and to
produce a rubidium-derived 1 PPS. It has not been built or ordered. JLCPCB, 4-layer.

## Why

- **Input conditioning.** Today a zero-centred sine clamps on the Si5351 CLKIN
  protection diode on every cycle ([hardware](hardware.md#si5351c-b-breakout)). That
  runs sustained current through a part not meant for it, and puts the threshold on a
  temperature-dependent diode knee.
- **A ÷10⁷ output is the main reason for the board.** A rubidium PPS on the F9T's EXTINT
  gives continuous UBX-TIM-TM2 time-marks of the AR-40A against GNSS. That is a running
  drift measurement, replacing the manual's oscilloscope phase-drift method.

## Circuit, if fed from a DA output (1.245 Vpp)

- **Input:** 75 Ω termination, jumper-selectable to 50 Ω. That covers both outcomes of
  the pending DA load measurement.
- **Squarer: a comparator is required.** 1.245 Vpp biased at VDD/2 swings 1.03–2.27 V,
  reaching neither the Si5351's V_IL (0.99 V) nor its V_IH (2.31 V). A CMOS inverter
  (e.g. 74AHC1GU04) is no better. **LTC6957-4** is the purpose-built part. ADCMP600 or
  LT1719 would also work.
- **Bias:** 100 nF C0G in series, then a matched 10k/10k divider to VDD/GND with 1 µF on
  the tap. The ratio sets the threshold, so the tempcos must track.
- **Supply:** a dedicated low-noise LDO (ADP151 / TPS7A20 / LP5907) with a ferrite and
  10 µF at its input, shared with nothing that switches. The threshold follows VDD
  directly.
- **Outputs:** squared 10 MHz → Si5351 CLKIN through 33 Ω in series. ÷10⁷ → 1 PPS → F9T
  EXTINT, using 4 × 74HC390 or a GreenPAK.
- **Layout:** ground plane under the input, bias network tight to the pin, both sides
  poured and stitched. Controlled impedance is unnecessary at 10 MHz over a few cm.

**Thermal budget.** At 1.245 Vpp the zero-crossing slew is ~39 V/µs, so 1 mV of threshold
shift is ~26 ps. Even 1 mV/°C across a 20 °C swing is ~0.5 ns of slowly varying phase,
around 10⁻¹³ fractional, orders of magnitude below the AR-40A's own ±2×10⁻¹⁰ temperature
coefficient.

## Decide before layout

These three choices are really one decision:

1. **Enclosure.** Does the squarer/divider share a box with the Si5351 and Teensy? If so,
   EXTINT becomes internal and only 10 MHz in, 5 V in, 54 MHz out and UART cross the
   panel. Every fast edge would stay on one ground plane, and most of the
   [grounding](grounding.md) concerns go away.
2. **Feed point.** A DA output, or straight from the AR-40A? Feeding direct gives 2.22 Vpp
   (~1.7× the slew, less threshold-noise jitter) and removes the board's ground from the
   Extron's mains earth, but it needs a second output or a splitter at the rubidium.
3. **54 MHz drive.** Should CM4 XIN eventually get a proper receiver instead of 10 Ω into a
   clamping input? That waits for a spare CM4. Meanwhile, treat the 54 MHz path as a
   driver into a defined load, and leave the series footprint able to take more than a
   single resistor.
