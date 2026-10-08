# The pulse-to-GPS tie: `ppp-vs-utcmike`'s premise was wrong, and what replaced it

**Status: resolved 2026-10-08, the same day it was raised.** The tie is now
`PPS − GPS = (receiver's clock bias − PPP's clock bias) − qErr`, implemented in
`tools/ubx-rxclock` and `tools/ppp-vs-utcmike --rxclock`. On the 10-03 session, against an
RTKLIB PPP clock with ESA finals, it gives **PPS − GPS = +0.7 ns, sd 3.0 ns (GPS only) and
+2.2 ns, sd 6.0 ns (GPS+Galileo)**, before the uncalibrated antenna/LNA/cable delay.

The first number quoted that day was **−17.5 ns, against the CSRS-PPP clock, and it was wrong**.
CSRS put the 10-03 antenna 8.5 m west of where its own data places it, and that error leaked
about +21 ns into its clock ([why](#why-the-10-03-csrs-position-was-85-m-off)). The note runs in
the order things were found: the morning's diagnosis, the
[−qErr reading](#the-simpler-reading-and-why-it-was-wrong) it proposed, which was also wrong, then
[the tie that works](#the-tie-that-works) and the position problem that tie exposed.

## What this is blocking

The UTC(MIKE) comparison. MIKES publishes `mikeYYMM.gpi` with 5-minute values of
REFGPS = UTC(MIKE) − GPS time, from a calibrated station. Differencing our own PPS − GPS against
theirs gives our output against UTC(MIKE), with satellite errors largely common to both. Everything
else for the October session is in hand: see `../../sys/state.md` and
`tools/ppp-vs-utcmike`.

## The data that provoked it

The 2026-10-03 session finally has a good PPP solution. Submitted a third time on 2026-10-08 and
processed against **multi-GNSS rapid** products, where the two earlier attempts had only
ultra-rapid:

```
SP3/CLK  EMR0MGBRAP_20262760000 / _20262770000   (rapid, multi-GNSS)
OBS      E C1C L1C   +   G C1C C2L L1C L2L       (Galileo present)
EPO      3011 3020 3113                           (99.7 % of epochs used; was 383/3002)
OFF      942411.5745 +- 2.1432 ns
IAR      GAL OFF, GPS 0.00 %
sigmas   X 0.316  Y 0.456  Z 0.317  H 0.358 m (95 %)
```

Coordinates go to `clock-private/`, as always. The 09-12 final run had tighter sigmas
(0.205/0.206/0.239/0.270) as a final solution with the same `IAR 0.00 %` should.

So the input is no longer the suspect: 3011 clock records at 30 s across 25.9 h.

## Defect 1: the sign self-check cannot discriminate, by construction

`pps_minus_gps()` builds the series for both signs and scores them with

```python
scored = {sign: abs(drift_ppm(pts)) for sign, pts in out.items() if len(pts) > 2}
```

Measured on the rapid `.clk`, 90685 matched pulses:

```
sign -1:  signed drift -0.062556 ppm      abs 0.062556
sign +1:  signed drift +0.062556 ppm      abs 0.062556
```

Two series differing only in the sign of one term produce drifts of equal magnitude and opposite
sign, so **`abs()` makes them score identically every time**. The check can never choose a sign.
This is independent of data quality — the sparse ultra-rapid `.clk` on 10-05 produced the same
symptom (+0.0466 ppm both ways) and the dense one reproduces it.

## Defect 2: the premise the check rests on

From the docstring:

> The receiver clock is a free-running TCXO drifting by ~0.46 ppm ... while a steered pulse sits
> within nanoseconds of GPS time. So of the two possible signs, exactly one cancels that drift and
> the other doubles it.

Those two sentences contradict each other. If `PPS − tow` carries no drift and the clock carries
0.0626 ppm, then adding or subtracting the clock gives ±0.0626 ppm and **neither cancels**.
Cancellation requires `PPS − tow` to already carry the TCXO's drift.

It does not. From `ubx-timtp` on the same session, 90758 pulses:

```
qErr: -4.05 .. +3.81 ns, mean -0.126 ns, sd 2.26 ns
time base GNSS, reference GPS
```

Bounded by a few nanoseconds, no drift. And the tool's own two fallback explanations are both
excluded by the files themselves:

- *"the .clk epochs and TIM-TP are both GPS time"* — the RINEX CLOCK header says
  `GPS ... TIME SYSTEM ID`, and TIM-TP's flags report time base GNSS / reference GPS. Both are GPS
  time. The assumption holds.
- *"the AR records are this receiver's clock"* — they are, and they behave like a free-running
  TCXO (−9358.4 to +942.4 µs across the session, ~0.11 ppm).

So the excluded explanations are the ones the tool offers, and the one it does not offer is the
premise itself.

## The simpler reading, and why it was wrong

As first written, this section argued that `towMS` is the GNSS-time instant the pulse was aimed at
and `qErr` is how far the hardware missed, so `PPS − GPS = −qErr`, the receiver clock never enters,
our side is "−0.126 ns ± 2.26 ns" from TIM-TP alone, and the `--clk` path should be deleted.

That is half right. The receiver does compensate its clock offset when it places the pulse, but it
compensates with **its own estimate** of that offset, not the truth. So `−qErr` is the pulse
against *the receiver's idea of* GPS time:

```
PPS − GPS  =  −qErr  +  (receiver's clock-bias estimate − true clock bias)  +  hardware terms
```

The qErr numbers say this themselves. A spread of −4.05 … +3.81 ns with sd 2.26 ns is a uniform
sawtooth over the ~8 ns hardware grid (8/√12 = 2.31 ns): quantisation and nothing else. Its mean
is the mean of a uniform distribution. Quoting it as our side would have been the receiver vouching
for itself, and differencing it against MIKES's REFGPS would have returned MIKES's number plus
noise and said nothing about our pulse. The middle term is the one worth measuring, and **PPP is
the only independent measurement of it**, so deleting `--clk` would have deleted the comparison.

## The tie that works

Both clock biases are receiver clock − GPS time: the receiver's own, from the log, and PPP's,
from the `.clk` AR records. Both carry the TCXO's ms-scale offset and its ~0.48 ppm drift, so these
cancel by subtraction. Nothing is fitted and no sign has to be guessed.

**The receiver's estimate was already in the 10-03 log.** `UBX-NAV-CLOCK` was not logged, but
`RXM-RAWX` gives each epoch in receiver time (`rcvTow`), and `NAV-TIMEGPS` gives the same epoch in
GPS time (`iTOW + fTOW`). Their difference is the clock bias. `tools/ubx-rxclock` extracts it.
CSRS-PPP tags AR records with exactly those `rcvTow` values (`00:42:59.991`), receiver time despite
the header's `GPS` time system, so 3009 of 3011 records pair without interpolation.

**The sign check now discriminates, on real data.** The receiver's bias minus PPP's has a median
of **−17.6 ns**. With the sign flipped it is **−464 864 ns**. The tool refuses anything over 1 µs,
and a deliberately negated `.clk` is refused with exit 1. This is unlike the old `abs(drift)`
check, which could not tell the two apart.

```
receiver − CSRS-PPP bias    3009 epochs   mean −17.5 ns   sd 8.9 ns   −10.8 ns/day
  change over   30 s  sd 5.2 ns     300 s  sd 7.3 ns     1800 s  sd 8.2 ns
+ (−qErr)                                  mean −17.5 ns   sd 9.2 ns
NAV-TIMEGPS tAcc (receiver's own claim)    median 1 ns
```

Against the CSRS clock, then, the pulse looked **17.5 ns early**, wandering ~9 ns over a day
while the receiver reported 1 ns. Neither figure survives a correct PPP position: against RTKLIB
the same tie gives +0.7 ns at sd 3.0 ns. See [below](#why-the-10-03-csrs-position-was-85-m-off).

One trap cost an hour and is recorded so it does not cost another: a double holding seconds since
1980 resolves only ~238 ns. Computed that way, the same data gave sd 97 ns with a triangular step
histogram, which looks exactly like a noisy receiver. `ubx-rxclock` and the tie work in integer
ms and ns throughout.

### What the tie does and does not include

- **Included, and cancelling:** the real antenna/LNA/cable delay *d* enters both biases equally,
  because both solutions see the same delayed signals. That is why the tie is clean.
- **Not removed:** the pulse is advanced by `CFG-TP-ANT_CABLEDELAY` (40 ns) to compensate *d*. So
  the full statement is `PPS − GPS = (tie) + (d − 40 ns) + δ`. Here *d* is the true antenna, LNA
  and cable delay and δ is any receiver-side code bias between the F9T's signal set and PPP's.
  *d* − 40 ns is the uncalibrated LNA group delay, 10–30 ns in
  [timing.md](timing.md#bias-and-error-budget). It is still the dominant term, and it is
  larger than the tie itself.
- **Assumed, not yet shown:** that the pulse is placed using the same estimate NAV-TIMEGPS reports.
  `f9t-rawlog` now logs NAV-CLOCK too. Its `clkB` is a second view of the same estimate, and
  `ubx-rxclock` prints the two side by side when both are present. Neither can show from inside the
  receiver which estimate the TP hardware uses; only an external comparison can.
- **Position.** In TMODE fixed the receiver computes its clock against its configured position,
  and PPP computes its clock against its own. A position error leaks into a clock estimate as the
  mean, over the satellites in use, of each line of sight projected onto the error. Both errors
  therefore enter the tie, and the next section measures both.

## Why the 10-03 CSRS position was 8.5 m off

The antenna has not moved. Independent RTKLIB 2.4.3 static PPP with ESA final orbits and clocks
(`ESA0OPSFIN`), IGS20 ANTEX, ionosphere-free, estimated troposphere with gradients and a 15° mask
agrees with itself across both sessions and both constellation sets:

```
RTKLIB run          - CSRS 09-12 final (E / N / U, m)    - CSRS 10-03 rapid (E, m)
09-12 GPS           +0.17 / -0.23 / +0.05                 +8.74
09-12 GPS+GAL       -0.01 / +0.24 / -0.39                 +8.56
10-03 GPS           -0.17 / -0.08 / +0.03                 +8.40
10-03 GPS+GAL       -0.27 / +0.09 / -0.63                 +8.29
```

All four agree within 0.45 m, and with CSRS's 09-12 final. **CSRS 10-03 is 8.5 m west and 2.5 m
high** of the truth they define. The configured position (CSRS 09-12 rapid) is 0.9 m west and
1.1 m high.

**Why one PPP run can be pulled that far: the sky makes east the weak axis.** The usable sky is a
wedge from 190° to 330° azimuth ([hardware.md](hardware.md#what-the-antenna-can-actually-see)).
With every phase-capable satellite to the west, moving the antenna east lengthens all their ranges
by about the same amount, which looks like a clock change. From NAV-SAT geometry, with the clock
eliminated per epoch and only C/N0 ≥ 35 satellites:

```
sigma ratio E : N : U : trop     open sky 0.67 : 1 : 5.1 : 1.8     this sky 2.62 : 1 : 7.0 : 2.0
correlation                      E-U -0.56   E-trop +0.44   U-trop -0.95
```

East goes from the best-determined horizontal axis to 2.6× worse than north, and is coupled to
height and wet delay. A bias anywhere in the measurements therefore comes out mostly in east and
up, which is exactly the shape of the 10-03 error.

**What supplies the bias: code from the shadowed sky.** East of the 185° building edge the signals
are diffracted, 14–25 dB-Hz with no phase. They still give pseudoranges, and those are biased.
CSRS shows it in its residuals: Galileo, which it takes as **E1 only** (C1C/L1C, E5b dropped), has
code RMS **25–30 m** and phase residual exactly 0.000 in both sessions. GPS code is 3–4 m, against
1.8 m in the GPS-only 09-12 final. Single-point solutions with RTKLIB isolate the effect:

```
SPP median E vs truth (m)      all satellites     C/N0 >= 35 only
09-12  GPS+GAL                 -8.86              -0.73
10-03  GPS+GAL                 -4.63              -1.22
09-12  GPS                     -8.85              -0.51
10-03  GPS                     -7.11              -1.23
```

The shadowed code pulls the solution 4–9 m **west**, the same direction as CSRS 10-03's error.
Masking it brings every case to about 1 m. Why CSRS was pulled further on 10-03 than on 09-12 is
not established. The satellite tracks through the shadow differ between the sessions, and so does
how much weak Galileo E1 code got in. The fix is the same either way: do not let shadowed code
drive this station's position.

**What it did to the tie.** Projecting each position error through the 10-03 geometry predicts
+2.7 ns of leak into the receiver's clock (configured position, satellites it used). It predicts
+12.5 to +22.7 ns into CSRS's clock, depending on whether the solution leans on all used satellites
or only the phase-capable ones. Measured directly:

```
PPP clock                          epochs   PPS − GPS          CSRS clock − this clock
RTKLIB GPS,     after 6 h          148      +0.7 ns sd 3.0     +20.6 ns
RTKLIB GPS+GAL, after 6 h          514      +2.2 ns sd 6.0     +21.5 ns
RTKLIB GPS, C/N0 >= 35, after 6 h  234      +5.0 ns sd 4.0     +24.7 ns
RTKLIB GPS+GAL, C/N0 >= 35, 6 h+   1537     +2.8 ns sd 4.6     +22.0 ns
CSRS 10-03 rapid                   3009     −17.5 ns sd 9.2    —
```

Across the four RTKLIB variants the tie is **about +3 ± 2 ns**. The spread comes from processing
choices, not from noise. The SNR-filtered GPS+Galileo run is the most complete: filtering out the
shadowed satellites lets RTKLIB solve 1856 epochs instead of 585, and its position sits within
0.3 m of the five-run mean.

The RTKLIB tie is the one to believe, with four caveats. It covers only 176 and 585 of 3011
epochs, because RTKLIB needs dual-frequency phase and this sky often leaves too few satellites. It
applies no C1C/C2L code-bias corrections against ESA's C1W/C2W clock datum, which is worth a few ns
of offset and some of the scatter. The two product sets' time scales may differ at the ns level.
And it still includes the configured position's own ~+2.7 ns leak, which is genuinely in the
pulse.

**One trap, recorded so it is not hit again.** RTKLIB 2.4.3 b34 reads the SP3 satellite count from
two columns (`str2num(buff,4,2)` in `preceph.c`), so a modern multi-GNSS file's `115` becomes `11`,
and most satellites report `prec ephem outage`. A one-character fix (`str2num(buff,3,3)`) was
applied to `~/ppp/RTKLIB` on ai on 2026-10-08 (uncommitted in that checkout), and `rnx2rtkp`
builds without the Fortran IERS library via `make LDLIBS="-lm -lrt"`.

## How this got past review

`timing.md` records the tool being validated on synthetic data: "a planted 25.0 ns offset and
0.456 ppm drift ... recovered +25.0 ns at 2.5 ns sd with 0.0000 ppm residual, the wrong sign
reported 0.9120 ppm — exactly twice the planted drift". That synthetic series was **generated with
the premise built in** — `PPS − tow` was given the clock's drift — so the test confirmed the
assumption instead of testing it. The `abs()` defect is invisible in that test too, because planting
a drift in the pulse term makes the two signs genuinely unequal in magnitude.

Worth remembering as a pattern: a self-check validated only against data you generated from the same
assumption is not a self-check.

## What was done (2026-10-08)

1. `tools/ubx-rxclock` (new): the receiver's own clock bias per epoch, from RAWX + NAV-TIMEGPS,
   with NAV-CLOCK alongside when logged, to CSV.
2. `tools/ppp-vs-utcmike`: the sign search and the `abs()` scorer are gone. `--timtp` now also
   needs `--rxclock`, and the tie is the subtraction above, with the 1 µs sanity refusal. `--clk`
   stays.
3. `tools/f9t-rawlog`: enables, disables and watchdogs `CFG-MSGOUT-UBX_NAV_CLOCK_UART1`
   (28 bytes/s, nothing against the UART budget), and `status` reports it. **Not yet deployed to
   aika**, and it takes effect from the next session.
4. Still open: settling qErr's sign from u-blox's interface description rather than from our own
   regression (`tools/ubx-timtp`). At ±4 ns it moves the mean by under 0.2 ns, so it is not urgent.
5. The 10-03 position question is answered above. Follow-ups:
   - **The configured position** was about 1.4 m off (0.94 m west, 1.06 m high), worth ~+2.7 ns
     in the pulse. The five-run RTKLIB mean is written into `clock-private/f9t-ppp-position.txt`
     and `f9t-known-good.txt`. **Applying it to the receiver is pending a go and a time window**,
     because it moves the pulse.
   - **CSRS submissions** go through `tools/rinex-filter` first. For 10-03,
     `aika-20261004-0237-GE35.obs.gz` (GPS+Galileo, C/N0 ≥ 35) and `-G.obs.gz` (GPS only) are in
     `~/ppp/2026-10-03/rinex/` on ai, ready to submit. Cross-check any CSRS position against
     RTKLIB with ESA finals before using its clock.
6. Still open, and the dominant term: the LNA delay. Do not let a tidy nanosecond-level tie imply
   the comparison is good to nanoseconds.

## Do not

- Do not quote a UTC(MIKE) number without the `(d − 40 ns)` caveat attached. The tool prints it.
  The October number needs `mike2610.gpi`, due mid-November.
- Do not read the ±0.0626 ppm residual of the old tool as a receiver or rubidium fault. It was the
  TCXO entering a calculation that did not subtract it.
- Do not take `−qErr` alone as PPS − GPS. It is the pulse against the receiver's own clock
  estimate.
- Do not tie against a PPP clock whose position has not been cross-checked. On 10-03, an 8.5 m
  position error turned +0.7 ns into −17.5 ns.
