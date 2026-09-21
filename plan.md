# Plan

Working document. What happens next, in order, with what to expect from each step.
Background and reasoning live in `README.md` and `docs/`; this is the checklist.

Last updated 2026-09-14.

---

## Now

- **The CM4 runs Arch Linux ARM** with a self-built PREEMPT_RT 7.2 kernel since
  2026-09-14; Ubuntu is gone. Paths and services below are the Arch ones.
- Steps 1 and 2 are done (PPP resubmission still pending). F9T TIME2 is locked-only
  since 2026-09-14.
- The AR-40A frequency record is building up undisturbed. Leave the trimmer alone
  (Step 3b).

---

## Step 1 — DONE 2026-09-11

Finished and verified in Flash:

```
lat 60.179612200   lon 24.958719200   height 22.138 m HAE
FIXED_POS_ACC 1.432 m  ->  4.7 ns        ECEF fields cleared
```

That is 3.6 m north, 1.5 m east and 3.1 m up from the old stored position — the
antenna move, measured. Timing came back at stratum 1, 16 ns RMS offset, NMEA
cycle 53 ms. Result saved as `config/survey-2026-09-11.meta`.

It is weaker than planned. The loggers are gpsd clients, and the gpsd restart for
the baud change dropped them at 03:54, so the run covered 3 h 48 min of its 10 h
and then held the receiver in rover mode for nine hours collecting nothing. The
loggers now run in a retry loop. **4.7 ns is comparable to the ~16 ns it replaced,
not dramatically better** — but it is centred on the truth with a known
uncertainty rather than confidently pointing where the antenna used to be. PPP in
Step 2 supersedes it.

<details><summary>Original Step 1 instructions</summary>

## Step 1 — finish the survey (~10:07)

```
f9t-survey finish
```

Writes to Flash: `LAT`/`LON`/`HEIGHT` plus their `_HP` fields, `FIXED_POS_ACC` from the
realistic accuracy, `POS_TYPE=1`, `MODE=2`. Clears the stale `ECEF_X/Y/Z` values. Turns
`RXM-RAWX` and `RXM-SFRBX` back off and stops the loggers.

Then confirm the rig came back:

```
gpsstat                 # expect stratum 1, PPS2 selected, exit 0
```

**Expected gain:** the stored position is currently stale — the antenna was moved, and
early rover fixes put it several metres away, i.e. of order **19 ns** of bias. After
this it should be ~1.3 ns.

Copy the resulting `~/f9t-survey/survey.meta` into `config/` and commit it. The surveyed
position belongs in git.

---

</details>

---

## Step 2 — DONE 2026-09-12, resubmitted 2026-09-19 on rapid products

23.2 h of RAWX at 30 s through NRCan CSRS-PPP. The rapid-product run is what the
receiver now holds, written to Flash and verified 2026-09-19. Reports kept as
`config/ppp-2026-09-12.sum` (ultra-rapid) and `config/ppp-2026-09-12-rapid.sum`,
keys as `config/f9t-ppp-position.txt` and again in `config/f9t-known-good.txt`.

```
lat 60.179598658   lon 24.958725025   hgt 22.0279 m ellipsoidal (ITRF20)
sigma 0.38 m 3D (1 sigma, east widened)  ->  1.28 ns     FIXED_POS_ACC 3840
```

**The first run was weaker than this plan said.** Its `EPO 410 2769 2787` line
means 410 epochs processed out of 2769, about 3.4 h, not the 23.2 h claimed here.
The rapid run processed 2768 and landed 1.27 m away — 0.04 m N, 0.97 m W, 0.82 m
up — which is 5.7 of the first run's own east sigmas. Its 0.41 m was not honest.

**The resubmit did not deliver what this plan promised either.** Rapid products
were supposed to bring Galileo and fix ambiguities. What came back:

```
IAR GPS 0.00%        still float
IAR GAL OFF          Galileo in, but E1 only; CSRS-PPP does not take the F9T's E5b,
                     and the E1 code residual is 30 m RMS
ANT NOT FOUND        no antenna phase-centre model (harmless, see below)
```

The limit is the data, not the products. 47% of GPS and 48% of Galileo
observations in the RINEX carry no L1 carrier phase, all of them weak signals:
mean 24 dB-Hz, against 41 dB-Hz where phase is present. Only 7655 of 24177 GPS
observations are fully dual-frequency and CSRS used 7253 of them, about 2.6 GPS
satellites per epoch. CSRS gives
0.19 m 3D, but from 17:00 on the first day the forward filter still wanders 0.69 m
east while holding 0.33 m north and 0.23 m up, so east is carried as 0.35 m and
the position as 0.38 m.

Final products (~2026-09-30) are an optional pass and should move little.

**What is actually in the way is the antenna's sky**, measured 2026-09-19 with
`tools/f9t-skymap` and written up in
[hardware](docs/hardware.md#what-the-antenna-can-actually-see). The half of the sky
from N clockwise to SSE returns 0-2% carrier phase at every elevation, so no product
tier can resolve ambiguities from this log, and the east coordinate is the weak axis
because east is the blocked side. A 10-degree azimuth cut puts a hard edge at ~185
degrees — 9 dB in one bin, which is a building corner, not an antenna pattern — and
a second obstruction sits overhead, costing 15 dB above 75 degrees elevation. The
antenna should stay level; the win is moving it clear of what is above it.

Anything that changes the antenna needs a fresh 24 h RAWX log and a re-run of
`f9t-skymap`; that is the measurement that says whether it helped. A multi-day log
would also buy geometry, but only after the sky itself is as good as it will get.

> **Trap:** `f9t-restore` runs at boot from `config/f9t-known-good.txt`, which
> carries the TMODE keys too. Change the position in one file and not the other and
> the next boot quietly puts the old one back.

On ANT NOT FOUND: a timing receiver in TMODE fixed computes ranges to the antenna
phase centre, so an uncorrected APC position is the self-consistent choice here.

**Correction to an earlier claim in this plan: PPP does not yield the absolute PPS
bias.** The clock output is the receiver's own free-running TCXO — `OFF
11109181 ns`, `DRI 39694531 ns/day`, which is 0.46 ppm and textbook for a TCXO.
PPP solves the receiver clock from pseudoranges, while the F9T corrects TP2
against its own solution, so the antenna and receiver delays do not fall out of
it. Measuring those needs a calibrated counter against a second reference, or
common-view against a laboratory. The antenna LNA and receiver delays therefore
remain uncompensated inside `CFG-TP-ANT_CABLEDELAY=40`, and remain unmeasured.

<details><summary>Original Step 2 instructions</summary>

## Step 2 — PPP, for the number nothing on the rig can measure

```
f9t-rawlog status       # coverage, logger health, UART margin
f9t-rawlog stop         # when it has run long enough
f9t-ppp ~/f9t-rawlog/rawx.ubx
```

Raw observations are independent of the navigation solution, so this needs **no
rover mode**: the receiver stays in TMODE fixed and the PPS keeps full timing
performance throughout. That is why this did not have to be part of the survey.

`f9t-rawlog` measures the NMEA cycle latency before and after enabling RAWX and
backs the change out itself if it eats the second-numbering budget. On this rig at
115200 it went 53 ms → 106 ms, 21% of the 500 ms budget. At 38400 the same change
consumed 184% of it, which is what broke the clock on 2026-09-11.

```
f9t-ppp                 # RAWX -> RINEX 3.04, gzipped
```

Submit the `.obs.gz` to **NRCan CSRS-PPP**, static mode, **ITRF** (not NAD83):
<https://webapp.csrs-scrs.nrcan-rncan.gc.ca/geod/tools-outils/ppp.php>

10 h of dual-frequency data is a solid dataset; 6 h is the usual minimum.

Two things come back, and the second is the point:

**a. Coordinates, at centimetre level (~0.1 ns).** Write them in with:

```
ubx-apply-config <keyfile> 7
```

No second survey — this is a config write against a receiver already in fixed mode.

> **Trap:** `CFG-TMODE-HEIGHT` wants height **above the ellipsoid**, not above the geoid.
> At this latitude the geoid separation is ~17 m, so confusing them is a **~57 ns** error
> — worse than everything fixed on 2026-09-10 combined. The PPP report gives both; take
> the ellipsoidal one.

**b. The receiver clock series.** Its offset against the PPP timescale is the rig's
**absolute time bias**, and it is the only way to get that number. chrony cannot supply
it: chrony steers the system clock to the PPS, so a constant offset is absorbed by the
servo and `sourcestats` reads `Offset -0ns` regardless (docs/timing.md).

That measured bias includes the **antenna LNA group delay** (typically 10–30 ns,
unspecified for this antenna) and the receiver's internal delay, both currently
uncompensated. If it shows a consistent offset, fold it into `CFG-TP-ANT_CABLEDELAY` on
top of the 40 ns cable term — converting those delays from unknown to measured.

</details>

---

## Step 3 — the RTC, whenever convenient

The single outstanding *hardware* fault, and the root of the outage on 2026-09-10.

1. **Fit a backup cell** to the CM4 IO Board's RTC connector. The PCF85063 reports
   `Power loss detected, invalid time`, so every cold boot comes up at a stale time,
   chrony steps the clock, and gpsd silently stops writing SHM(0) — taking both GNSS
   refclocks down with it. `rtcsync` is already in `chrony.conf`, so the RTC will be
   correct as soon as it can hold charge. **Still open.**
2. ~~Install `fake-hwclock`.~~ Not packaged on Arch, and no longer needed: `fixrtc` was an
   Ubuntu initramfs option and is gone, and systemd raises a clock that reads earlier than
   its build epoch at boot, so a flat cell costs a step of months at most, not years.
3. ~~Re-arm gpsd after any step.~~ **Done on Arch 2026-09-14:** `gpsd-after-timesync.service`
   runs `systemctl try-restart gpsd` after `time-sync.target`, and `gpsd-shm-watchdog.timer`
   restarts gpsd if SHM(0) stops (every minute, from 5 min after boot). Do *not* order
   gpsd after `chrony-wait` instead: with the network down at boot, chrony has no source,
   `chrony-wait` blocks, and gpsd — the only remaining time source — never starts.

`gpsstat` still reports the missing cell as a `NOTE` on every run so it does not get
forgotten.

---

## Step 3b — trimming the AR-40A: DONE 2026-09-21

**−2.5510×10⁻⁹ → +2.80×10⁻¹¹ in four moves, a factor of 91.** Holdover drops from
220.1 to 2.4 µs/day. Total travel 5.25 turns of ten, all clockwise, with no endstop
reached. Full record in `config/ar40a-trim-log.txt`.

**Finished, and not because it ran out of adjustment.** The residual is 0.90× chrony's
own skew, so what is left is smaller than the uncertainty it is measured with, and
about 3× the 8×10⁻¹² the oscillator wanders by overnight unaided. Going further needs
a better reference, not more turns.

What the trimming established, none of which was known before:

- **Forward is clockwise, and it raises frequency.** Nothing had recorded this.
- **4.89–4.92×10⁻¹⁰ per turn**, against the manual's 5×10⁻¹⁰ — spec good to 2%, and
  linear across 4.5 turns, so the endstop was never reached and travel remains.
- **The rig does not notice.** Stratum 1 throughout, PPS2 +0 ns, stddev 20 ns, across
  a 2×10⁻⁹ frequency step. The step is in the rubidium; chrony's servo absorbs it.
- **No mechanical creep so far.** The half turn drifted 2×10⁻¹² in the following hour;
  the four-turn move settled in 38 min at a skew *below* its own starting value. The
  warning below about creep for hours to days is not what this unit does.
- **Skew is the settle indicator, not the offset.** The offset reads nearly right
  within minutes; skew is what says chrony has converged and the number can be
  trusted. Watch for it to return to ~3×10⁻¹¹.
- **A reading is only valid if the drift file was written after the move.** Freshness
  alone is not enough — a file under an hour old can still predate the turn.
- **The kernel has the number live.** chrony steers the clock through `adjtimex(2)`,
  whose `freq` field is the correction being applied right now, in units of 2⁻¹⁶ ppm
  — 1.53×10⁻¹¹ resolution, against the 1×10⁻⁹ that everything `chronyc` prints. The
  rubidium's offset is that correction negated. `tools/rb-freq-live` displays it, so
  the trimmer can be turned against a number that moves in minutes rather than in
  hours. chrony dithers between adjacent LSBs, so read the rolling mean, not the
  instantaneous value; agreement with the drift file is better than one LSB.

The reasoning below is retained because its conclusion still stands: this buys
nothing for timekeeping, and it was done because it was there to do.

---

**Original assessment, 2026-09-14: leave it alone for now, and probably for good.**

### What the frequency is doing

`ref_freq_offset` from the dashboard history, 6 h means (chrony drift file, AR-40A vs
GNSS):

| Window (UTC) | Offset | chrony skew |
|---|---|---|
| 09-11 00 | −2.590×10⁻⁹ | 3.5×10⁻¹⁰ |
| 09-12 00 | −2.529×10⁻⁹ | 1.2×10⁻¹⁰ |
| 09-13 06 | −2.556×10⁻⁹ | 8.9×10⁻¹¹ |
| 09-14 12 | −2.532×10⁻⁹ | 3.1×10⁻¹¹ |

−2.55×10⁻⁹ ± ~5×10⁻¹¹ since the 2026-09-11 power cycle, with no retrace trend visible
after 3.5 days. Left out: the disturbed 09-11 18 window (skew up to 8×10⁻⁹) and 09-13 18 →
09-14 06, when the OS migration stopped the drift file being rewritten (up to 19 h stale).

**Read the drift-file figure, never `chronyc tracking`.** `tracking` and `tracking.log`
print ppm with three decimals, i.e. 1×10⁻⁹ steps, so they show a flat `0.003 ppm slow`
while the rubidium moves by up to ±5×10⁻¹⁰. The drift file has 1×10⁻¹² resolution
(`ref_freq_offset`); `ref_freq_live` is the rounded one.

### Correction: the offset does not cost holdover

An earlier version of this step said the offset drifts the clock by 224 µs/day once GNSS
is lost, and that trimming would cut that to ~4 µs/day. **That is wrong for the
timekeeping.** chrony carries the −2.55×10⁻⁹ in its frequency estimate, keeps applying it
when the refclocks go unreachable, and reloads it from the drift file after a reboot, so
the static offset never accumulates as time error. Holdover is limited by how much the
frequency *changes* after the reference is lost — aging, temperature, retrace — and the
trimmer does nothing about those.

Measured holdover (integral of f(t)−f(t₀) over 63 h of PPS2 lock, 2026-09-13) is p95
79 µs after 24 h, worst 332 µs. It was computed from the rounded `tracking.log` values, so
the real figure is likely better. The `holdover drift if GNSS is lost` line in `gpsstat`
is the static-offset number and overstates holdover the same way (Step 4).

PTP and NTP clients follow the corrected clock, so they gain nothing either. The offset
reaches only consumers of the raw 10 MHz, and 2.6×10⁻⁹ is invisible to the Orion (audio
wants ppm) and to bench instruments.

### What trimming would cost

- **Unknown trimmer position.** From the manual (docs/hardware.md): under the calibration
  sticker, **1 turn ≈ 5×10⁻¹⁰, 10 turns total**. Nulling −2.55×10⁻⁹ needs ~5.1 turns in
  one direction — right at the endstop if it was left near centre, and nothing records
  where it sits.
- **A fresh settle.** A just-moved trimmer can creep for hours to days: a known, stable
  offset traded for an unknown, moving one.
- **The aging record restarts.** Aging only shows over weeks; the undisturbed record
  began 2026-09-11.
- **Comparisons get spoiled.** A 2.5×10⁻⁹ step in the middle of an OS or kernel
  comparison run contaminates it.

### If it gets done anyway

1. Not before 2026-09-18 (a week after the power cycle), and not during a comparison run.
2. **½ turn first, then wait ≥ 1 h** for a fresh drift file. That gives the direction and
   the real sensitivity. Frequency has to go up (the offset is negative).
3. **Count and write down every turn**, then continue in ~1-turn steps. Add the record to
   `config/`.
4. Judge by `ref_freq_offset` with a fresh `drift file age`, not by `chronyc tracking`.

The offset itself is ordinary aging, not a fault. Spec is <1×10⁻⁹ the first year and
<5×10⁻¹⁰/yr after, so 2.55×10⁻⁹ implies at least ~4 years and realistically much longer —
the unit is behaving like an old rubidium that has not been recalibrated, which is what it
is.

---

## Step 4 — smaller items, no particular order

- ~~**NMEA `offset` is wrong by ~0.2 s.**~~ Overtaken by the 115200 baud change: on Arch
  `chrony.conf` carries no `offset` and NMEA reads ~+60 ms (2026-09-14), well inside the
  ±0.5 s second-numbering budget.
- ~~**UART1 at 38400 is the latency.**~~ **Done 2026-09-11, forced.** Enabling RAWX and
  SFRBX for the survey pushed the serial cycle from ~250 ms to ~919 ms, past the ±0.5 s
  that PPS second-numbering needs. chrony numbered the pulse to the wrong second and put
  the clock ~527 ms off UTC while still reporting stratum 1 — the servo was tracking the
  pulse perfectly, it was simply the wrong pulse. Raised to 115200; cycle now ~121 ms and
  the MIKES servers agree to under a millisecond. **Treat UART bandwidth as a correctness
  constraint, not a latency nicety**, and check the margin before enabling anything else
  on UART1.
- **Build the connectors:** 75 Ω terminator, 50 Ω terminator, BNC tee. Still the only
  thing gating the DA/Orion measurement, which now needs the scope carried to the rack.
  At 10 MHz a few cm of unmatched junction is invisible, so home-made parts are fine;
  what matters is the **value** — measure each resistor and use the measured figure.
  Build terminators as male plugs (they go on the free leg of the tee), leads short,
  resistor body inside the shell.
- **DA output into a real 75 Ω load, and Orion's actual termination** — one procedure,
  three readings at the Orion input, same setup and probe:
  1. Cable open → **V_open**, the DA's unloaded output.
  2. Known 75 Ω terminator on the free leg of the tee → ≈ V_open/2 means the build-out
     really is 75 Ω and the DA drives a real load without sagging.
  3. Orion connected → its input resistance is 75 × V_orion / (V_open − V_orion);
     ~1.26 V against ~2.5 V open means it terminates at 75 Ω.

  Not blocking the timing board: its input termination is jumper-selectable.
- **Teensy `dataErr` rate.** Rejected I2C readings still accumulate slowly in the rack,
  with 4.9 kΩ pull-ups fitted and the laptop disconnected. Rejections are safe (CLK4
  holds), but "slowly" is not a number: reset the Teensy, record errors per hour, then
  try a second 4.9 kΩ in parallel per line (≈2.1 kΩ) and compare.
- **Antenna inspection:** where the coax shield is bonded, and a gas-discharge arrestor
  at the entry point if the run is roof-mounted.
- ~~**`ntpsec` leftover** `ntploggps` cron entry.~~ Gone with Ubuntu (2026-09-14).
- **`gpsstat` holdover line** prints the static-offset drift (~224 µs/day), which chrony
  already corrects (Step 3b). Relabel it, or replace it with a measured holdover figure.

---

## Deliberately not doing

Recorded so these do not get re-opened by accident.

- **DGNSS in the steady state.** A base station does not consume corrections, and if it
  did they would carry the reference station's own clock offset straight into the time
  solution — making the rig traceable to a FinnRef clock rather than to UTC. It was only
  ever a survey tool. Full reasoning in docs/timing.md.
- **GLONASS.** FDMA, so each satellite carries its own receiver hardware bias.
  Calibrating inter-frequency bias is a real job and timing configurations routinely
  leave it out.
- **QZSS** — regional to Japan, nothing visible at 60°N. **SBAS** — adds nothing to a
  fixed-position dual-frequency receiver, and the EGNOS geostationaries sit at low
  elevation from here, where multipath is worst.
- **BeiDou B2I.** `CFG-SIGNAL-BDS_B2_ENA` reads 1 but no B2I observations appear in the
  RINEX, so BeiDou contributes satellites without an ionosphere-free combination.
  Probably a concurrency limit in the timing firmware. Costs nothing; left alone.
- **Restructuring so `ntrip-relay` owns the serial port** with gpsd behind a pty. It
  would make DGNSS injection work, but it inserts a new failure point into the data path
  for a service capped at 0.5 m — and see the first entry.
- **Characterising CLK4 amplitude at CM4 XIN.** Needs a second identical CM4 to
  experiment on (docs/hardware.md).
- **Boot-console bridge.** GPIO14/15 are occupied by the F9T.

---

## Dashboard interaction

`dashboard/` collects `gpsstat` every 5 minutes via `/etc/cron.d/aika-clock-dashboard`
and publishes to <https://www.mui.fi/clock/>. The 2026-09-11 survey shows as a degraded
period in the history (rover mode, expected).

- **`gpsstat` is on a schedule, so its exit code matters more than before.** This is
  why the RTC condition is a `NOTE` rather than a `WARN`: a known, accepted fault must
  not hold the dashboard at a permanent non-zero verdict, or a real failure stops
  standing out. Keep that distinction when adding checks — `note()` for accepted
  conditions, `warn()`/`fail()` only for things that need action.

---

## Reference

Tools, the error budget and recovery steps now live in [docs/timing.md](docs/timing.md)
and [docs/troubleshooting.md](docs/troubleshooting.md).

---

## What good looks like

After Steps 1 and 2, `gpsstat` should report:

```
chrony
  ref 50505332 (PPS2) | stratum 1
  OK    disciplined by PPS2 (GNSS 1 PPS via PHC)
  OK    refclock PPS2 reach 377
  OK    refclock NMEA reach 377

gpsd -> chrony SHM
  OK    SHM(0) live

GNSS fix
  OK    3D fix, 20+ sats used, PDOP < 2
  constellations in solution:  GPS n  Galileo n  BeiDou n

accuracy
  receiver time accuracy (NAV-TIMEGPS tAcc) : ~1 ns
  chrony PPS2 offset / std dev              : ~0ns ~20ns

hardware / boot
  NOTE  RTC has no backup cell        <- until Step 3
  OK    ser2net inactive
  OK    PHC /dev/ptp0 is bcm_phy_ptp

verdict
  GNSS timing chain healthy
```

Exit code 0. The RTC line stays a `NOTE` rather than a `WARN` deliberately — known and
accepted, so it does not mask conditions that are actually news.
