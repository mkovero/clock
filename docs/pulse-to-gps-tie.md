# The pulse-to-GPS tie: `ppp-vs-utcmike`'s premise is in question

**Status: open, raised 2026-10-08. Nothing in the tool has been changed.** It currently refuses to
quote a number, which is the correct behaviour, so there is no urgency and no wrong answer in
circulation. This note exists so the next person — or the next context — can pick the question up
without re-deriving it.

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

## The simpler reading, and what it would mean

`UBX-TIM-TP`'s `towMS` is the GNSS-time instant the pulse was *aimed at*, and `qErr` is how far the
hardware missed. If that is the whole story, then

```
PPS − GPS time  =  −qErr
```

and the receiver's internal clock offset never enters, because the receiver already compensates for
it when placing the pulse. The ms-level series PPP reports describes the receiver's *clock*, not its
*pulse*.

If that is right:

- Our side of the comparison has been available since the session was logged:
  **−0.126 ns ± 2.26 ns** over 90758 pulses.
- The `--clk` path in `ppp-vs-utcmike` is unnecessary for our side and should be removed rather than
  fixed.
- What stands between that and a quotable UTC(MIKE) number is only the **uncalibrated
  antenna/LNA/cable delay** — the 10–30 ns estimate in [timing.md](timing.md#bias-and-error-budget),
  for which u-blox publishes nothing on the ANN-MB. That is a calibration problem, not a software
  one, and it dominates everything else here.

## How this got past review

`timing.md` records the tool being validated on synthetic data: "a planted 25.0 ns offset and
0.456 ppm drift ... recovered +25.0 ns at 2.5 ns sd with 0.0000 ppm residual, the wrong sign
reported 0.9120 ppm — exactly twice the planted drift". That synthetic series was **generated with
the premise built in** — `PPS − tow` was given the clock's drift — so the test confirmed the
assumption instead of testing it. The `abs()` defect is invisible in that test too, because planting
a drift in the pulse term makes the two signs genuinely unequal in magnitude.

Worth remembering as a pattern: a self-check validated only against data you generated from the same
assumption is not a self-check.

## Before changing anything

1. **Settle what `towMS` is referenced to**, from u-blox's interface description rather than from
   our own code comments. The question is whether the F9T aims TP1 at the GNSS second (so the
   receiver's clock error is already removed) or at its own clock's idea of it.
2. If the simpler reading holds, **delete the `--clk` tie rather than repair it**, keep `--timtp`,
   and state the result as −qErr with its uncertainty.
3. If it does not hold, **fix the discriminator first** — compare signed drift against zero, not
   magnitudes — and re-derive the synthetic test so it is generated independently of the hypothesis
   it checks.
4. Either way the antenna/LNA/cable delay stays uncalibrated and remains the dominant term. Do not
   let a tidy nanosecond-level tie imply the comparison is good to nanoseconds.

## Do not

- Do not quote a UTC(MIKE) number from the current tool. It exits rather than printing one, and that
  is correct.
- Do not read the ±0.0626 ppm residual as a receiver or rubidium fault. It is the TCXO behaving
  exactly as expected, entering a calculation that should probably not include it.
