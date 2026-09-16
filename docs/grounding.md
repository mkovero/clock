# Grounding

The design principle is **one low-impedance path between domains**. A fully floating
island is not possible here. Treat every added bond between islands as a change to test,
not as tidying.

## Domains

**Reference chain.** The AR-40A module case, SMA output ground and PSU ground are all
bonded to its enclosure, and that enclosure is **at mains PE** (continuity confirmed
2026-09-10). The Extron has a grounded IEC inlet. Both ends of the 10 MHz chain are
therefore earthed, and the coax shield closes a loop between them.

**Digital island.** Si5351 + Teensy, CM4 and F9T all run from the floating RS-15-5, so
they share DC ground through the supply wiring. The island is earth-referenced through
the 10 MHz coax shield from the DA, and that cannot be avoided: the shield has to bond to
board ground to act as the signal return.

## Paths

| Path | State |
|---|---|
| AR-40A chassis → mains PE | bonded (measured) |
| Extron chassis → mains PE → 10 MHz shields | bonded, second earth injection |
| DA output → Si5351 board | coax shield |
| Si5351 board → CM4 | 54 MHz coax shield, grounded both ends |
| Si5351/Teensy, CM4, F9T | common DC return through the RS-15-5 star |
| Teensy → CM4 | status UART ground (header pin 34) |
| F9T → CM4 | supply return, UART ground, PPS shield at the CM4 end only |
| AR-40A 15 V −V ↔ 5 V −V | **deliberately not connected** |
| GNSS antenna shield | bonding point unknown |

## Rules

- **Do not tie the LRS-75-15 −V to the RS-15-5 −V.** Both domains already reach earth
  (AR-40A chassis on one side, Extron and coax on the other), so a direct tie would add a
  second path. They exchange no signal that needs a shared DC reference.
- **Star the 5 V distribution.** The CM4 draws >1 A with fast transients, and a shared
  return would put that onto the ground that the Si5351 CLKIN threshold is referenced to.
- **Route the 54 MHz coax alongside the DC supply pair.** It forms a loop with the supply
  return, and keeping the two together minimises the enclosed area.
- **Isolate the Si5351 enclosure's BNC input from the enclosure.** Bond the shield to the
  board ground pour only.
- **Keep the PPS shield single-ended.** When it was bonded at the F9T end as well as at J2,
  **the F9T froze outright** — no PPS, no UART — and lifting that end restored it at once.
  Ground difference with the single bond is <10 mV, so the mechanism is unexplained. Don't
  reintroduce the bond. The CM4 boots from the Si5351's 54 MHz, not from anything the F9T
  provides, so this does not stop the CM4 coming up; it takes the time reference away.
- **Keep the LED strand shield off the chassis.** It is a conductor (the common cathode
  return), not a screen.
- **Never lift a safety earth.**
- **The pad enclosure's grounding does not matter.** The AR-40A output ground already is
  its earthed chassis. Isolated bulkheads only matter for boxes that bring a *new* earth
  reference, such as a rack-mounted timing board with its own earthed supply.
- **The BIT line needs no optocoupler.** It is a high-impedance path (a pull-up of tens of
  kΩ) running alongside the milliohm coax bond that already joins those grounds.

## Measurements

| Between | Reading | Date |
|---|---|---|
| F9T ground ↔ CM4 ground | <10 mV AC | 2026-09-09 |
| Teensy ground ↔ CM4 ground | 0.6 mV | 2026-09-09 |
| CM4 ground ↔ Extron chassis (in rack) | <3 mV AC | 2026-09-10 |
| RS-15-5 FG ↔ DC output | open | — |

**The earth-path question is closed for this build.** Both earth injections exist, but
the loop develops only single-digit millivolts. A few millivolts on a 1.245 Vpp, 10 MHz
sine is a mains-rate phase modulation of a few picoseconds that averages out and does
not affect frequency accuracy. The ground that matters most is the **PPS path**
(F9T ↔ CM4), because noise there shifts a single edge that nothing averages. That path
is benign at <10 mV.
