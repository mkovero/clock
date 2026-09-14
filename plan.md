# Plan

Working document. What happens next, in order, with what to expect from each step.
Background and reasoning live in `README.md`; this is the checklist.

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

## Step 2 — DONE 2026-09-12, and worth resubmitting

23.2 h of RAWX, 2784 epochs at 30 s with no gaps, through NRCan CSRS-PPP. Written
to Flash and verified; report kept as `config/ppp-2026-09-12.sum`, keys as
`config/f9t-ppp-position.txt`.

```
lat 60.179598272   lon 24.958742464   hgt 21.2083 m ellipsoidal (ITRF20)
sigma 0.41 m 3D (1 sigma)  ->  1.35 ns        FIXED_POS_ACC 4103
```

That is 2.22 m from the 2026-09-11 standalone survey, which had claimed 1.43 m —
so 1.6 sigma out, meaning the survey's own uncertainty estimate was honest if
slightly optimistic. Position-induced bias improves from roughly 4.7 ns to 1.35 ns.

**Resubmit the same file — but sooner than two weeks.** That figure was the
*final*-product latency, and it is the wrong advice for the first resubmit. The
submission went in two hours after the observation ended, so only the lowest
product tier existed:

| Product | Latency | What it buys |
|---|---|---|
| ultra-rapid | real-time / 6 h | what this run got: no Galileo, IAR failed |
| **rapid** | **~1-2 days** | Galileo included, far better orbits and clocks, IAR should fix |
| final | ~12-18 days | the last increment, fully reprocessed |

The error was specific — *no **ultra-rapid** Galileo products* — so rapid and final
both carry Galileo. Most of the gain is available within a couple of days; final
products are worth one more pass afterwards if the last increment matters.

Reminders for both are in `config/f9t-reminders` and surface through `gpsstat` on
the dashboard, so they do not depend on anyone remembering.

The solution is decimetre rather than centimetre only because of what was
available a day after observing:

```
IAR GPS 0.00%        ambiguity resolution failed; float solution only
Galileo dropped      no ultra-rapid Galileo products exist
EMR1DCBULT           ultra-rapid orbits and clocks, the least accurate tier
ANT NOT FOUND        no antenna phase-centre model
```

Final products fix the first three, and the file is already on hand at
`~/ppp/aika-20260912-0942.obs.gz`. The fourth matters less than it reads: a timing
receiver in TMODE fixed computes ranges to the antenna phase centre, so an
uncorrected APC position is the self-consistent choice here.

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
servo and `sourcestats` reads `Offset -0ns` regardless (README §10).

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

## Step 3b — trimming the AR-40A: measured, and not needed for timekeeping

**Leave it alone for now, and probably for good.** Measured 2026-09-14.

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

- **Unknown trimmer position.** From the manual (README §3): under the calibration
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

- **NMEA `offset` is wrong by ~0.2 s.** With `offset 0.028` the source reads +201 to
  +223 ms. Harmless today (NMEA is `noselect` and only numbers PPS pulses, needing
  ±0.5 s) but it spends half the ambiguity budget. Correct to ≈0.24 — **determine the
  sign empirically**, chrony's display convention is easy to invert — and do it
  deliberately, since it costs a chronyd restart.
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
- **DA output into a real 75 Ω load, and Orion's actual termination** — one procedure,
  three readings (README §7 item 8).
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
  ever a survey tool. Full reasoning in README §10.
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
  experiment on (README §8).
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

## Reference — the tools

| Tool | Purpose |
|---|---|
| `gpsstat` | one-screen health of the whole chain; exit 0/1/2 for cron |
| `f9t-survey` | `start` / `status` / `finish` / `abort` the position survey |
| `f9t-ppp` | RAWX → RINEX for PPP submission |
| `ubx-dump-config` | full CFG dump, both layers, paged |
| `ubx-apply-config` | apply a key/value file, verifying every write by readback |
| `ntrip-relay` | NTRIP client and local re-caster; unused in steady state |

Two traps these encode:

- **`ubxtool` must name its gpsd device** whenever more than one is attached, or every
  readback silently returns empty. This first appeared as a survey that *reported*
  disabling TMODE while changing nothing.
- **`UBX-CFG-VALGET` returns at most 64 items**, so dumping a large group must page with
  the `position` field. The first backup silently truncated `CFG-MSGOUT` to 64 of its
  561 keys.

---

## Error budget — where the nanoseconds are

State after the 2026-09-10 changes, the survey and PPP (2026-09-12).

| Term | Size | Status |
|---|---|---|
| Antenna cable delay | 35 ns | **fixed** — 5 → 40 ns, calculated not measured |
| Stored position | 1.35 ns | **fixed** — PPP 2026-09-12 (was ~19 ns, stale after the antenna move) |
| Antenna LNA group delay | 10–30 ns `[?]` | **uncompensated**, unmeasurable on the rig |
| Receiver internal delay | unknown `[?]` | **uncompensated**, unmeasurable on the rig |
| chrony PPS2 jitter (std dev) | 22 ns | random, averages out |
| Receiver time solution (`tAcc`) | 1 ns | was 3 ns before dual-frequency Galileo |
| Ionospheric residual | few ns | reduced by GPS L1+L2C and Galileo E1+E5b |

Two things worth reading off this table:

**The systematic terms dominate, and most are still unmeasured.** Cable delay, LNA delay
and receiver delay are constant offsets — they do not average out, and no amount of
observation on this rig reveals them. PPP does not either (Step 2 correction); it takes a
calibrated counter against a second reference, or common-view against a laboratory.

**The position work was polish, not repair.** It bought ~18 ns against a 35 ns cable fix
that cost one config write. The biggest remaining prize is the LNA and receiver delay
pair, and nothing on the rig can measure it.

---

## Recovery — if something goes wrong

**Receiver configuration is wrong:** every key in `config/f9t-config-ram.txt` is a
`<key> <value>` line, so a restore is `ubx-apply-config` against a filtered copy of that
file. The dump is complete — 945 keys, RAM and Flash byte-identical at the time it was
taken.

**GNSS refclocks unreachable after a cold boot:**

```
sudo systemctl restart gpsd
```

`gpsd-after-timesync` and `gpsd-shm-watchdog` do this automatically (Step 3); run it by
hand only if they did not. `gpsstat` will
show `Reach 0` on both refclocks and `gpsd is NOT writing SHM(0)` when this is the
problem. Note that `chronyc tracking` alone looks *healthy* in this state — a
well-disciplined stratum-3 clock — which is exactly why `gpsstat` exists.

**gpsd talking to the wrong device:** if `/etc/default/gpsd` ever lists more than
`/dev/ttyAMA0`, bare `ubxtool` calls fail with `No path specified in DEVICE`. Either
remove the extra device or pass `localhost:2947:/dev/ttyAMA0`; the tools do this already
via `$F9T_TARGET`.

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
