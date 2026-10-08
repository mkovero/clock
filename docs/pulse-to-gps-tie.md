# The pulse-to-GPS tie: `ppp-vs-utcmike`'s premise was wrong, and what replaced it

**Status: resolved 2026-10-08, the same day it was raised.** The tie is now
`PPS − GPS = (receiver's clock bias − PPP's clock bias) − qErr`, implemented in
`tools/ubx-rxclock` and `tools/ppp-vs-utcmike --rxclock`. On the 10-03 session it gives
**PPS − GPS = −17.5 ns, sd 9.2 ns, drift −10.8 ns/day over 3009 epochs**, before the uncalibrated
antenna/LNA/cable delay. **New blocker:** that PPP solution puts the antenna 7.8 m from the
receiver's configured position, which accounts for ~9 ns of the offset. Which position is right is
[open](#what-175-ns-does-and-does-not-include). The first half of this note is the diagnosis as written that morning.
[The −qErr reading](#the-simpler-reading-and-why-it-was-wrong) that it proposed was itself wrong,
and [the fix](#the-tie-that-works) follows it.

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
receiver bias − PPP bias    3009 epochs   mean −17.5 ns   sd 8.9 ns   −10.8 ns/day
  change over   30 s  sd 5.2 ns     300 s  sd 7.3 ns     1800 s  sd 8.2 ns
+ (−qErr)                                  mean −17.5 ns   sd 9.2 ns
NAV-TIMEGPS tAcc (receiver's own claim)    median 1 ns
```

So the receiver puts its pulse **about 17.5 ns early against GPS time as this PPP solution
realises it**. It wanders by ~9 ns over a day while telling us its time is good to 1 ns. About
half the offset may be position rather than timing; see the last bullet below.

One trap cost an hour and is recorded so it does not cost another: a double holding seconds since
1980 resolves only ~238 ns. Computed that way, the same data gave sd 97 ns with a triangular step
histogram, which looks exactly like a noisy receiver. `ubx-rxclock` and the tie work in integer
ms and ns throughout.

### What −17.5 ns does and does not include

- **Included, and cancelling:** the real antenna/LNA/cable delay *d* enters both biases equally,
  because both solutions see the same delayed signals. That is why the tie is clean.
- **Not removed:** the pulse is advanced by `CFG-TP-ANT_CABLEDELAY` (40 ns) to compensate *d*. So
  the full statement is `PPS − GPS = −17.5 ns + (d − 40 ns) + δ`. Here *d* is the true antenna, LNA
  and cable delay and δ is any receiver-side code bias between the F9T's signal set and PPP's.
  *d* − 40 ns is the uncalibrated LNA group delay, 10–30 ns in
  [timing.md](timing.md#bias-and-error-budget). It is still the dominant term, and it is
  comparable in size to the −17.5 ns itself.
- **Assumed, not yet shown:** that the pulse is placed using the same estimate NAV-TIMEGPS reports.
  `f9t-rawlog` now logs NAV-CLOCK too. Its `clkB` is a second view of the same estimate, and
  `ubx-rxclock` prints the two side by side when both are present. Neither can show from inside the
  receiver which estimate the TP hardware uses; only an external comparison can.
- **Position, and this is the new blocker.** In TMODE fixed the receiver computes its clock
  against its configured position. That position is the 09-12 rapid PPP solution. PPP computes its
  clock against its own position, and the 10-03 solution puts the antenna **7.8 m** from the
  configured one (ENU +7.60 / −0.88 / −1.48 m). That is 20× its own 0.36 m 95 % sigma. A position
  error leaks into a clock estimate as the mean, over the satellites in use, of each line of sight
  projected onto the error. Computed from NAV-SAT (19 satellites used, equal weights), the 7.8 m
  predicts **−9.3 ns** of the −17.5 ns. The time-varying part is small (sd 2.2 ns, r = +0.11
  against the observed series), so most of the 9 ns wander is something else.

  The PPP solutions disagree with each other more than with anything physical. 09-12 final and
  09-12 rapid agree to 1.3 m. 10-03 rapid is 8.6–8.9 m from both, mostly east. Nothing records the
  antenna moving after 2026-09-11 ([hardware.md](hardware.md)). So either 10-03's position is
  wrong, and its clock with it, or 09-12's was and the receiver has run on a bad position since.
  Until that is settled, **the −17.5 ns carries a ~9 ns position term of unknown sign**. The
  differences above were computed from `clock-private/` without printing coordinates. Two
  unexplored leads: 10-03's AR tags sit up to 9.4 ms off GPS time (receiver time), and the 09-12
  final used only 410 of 2769 epochs.

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
5. Open, and now first: why the 10-03 and 09-12 PPP positions differ by ~8 m, and so which
   position the receiver should be running on. Changing the configured position is aika work and
   shifts the pulse by nanoseconds, so it waits for a go.
6. Still open, and the dominant term once 5 is settled: the LNA delay. Do not let a tidy −17.5 ns imply the comparison is
   good to nanoseconds.

## Do not

- Do not quote a UTC(MIKE) number without the `(d − 40 ns)` caveat attached. The tool prints it.
  The October number needs `mike2610.gpi`, due mid-November.
- Do not read the ±0.0626 ppm residual of the old tool as a receiver or rubidium fault. It was the
  TCXO entering a calculation that did not subtract it.
- Do not take `−qErr` alone as PPS − GPS. It is the pulse against the receiver's own clock
  estimate, and that estimate was 17.5 ns off on 10-03.
