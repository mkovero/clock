# Plan

Working document. What happens next, in order, with what to expect from each step.
Background and reasoning live in `README.md`; this is the checklist.

Last updated 2026-09-11.

---

## Now — survey running

Started `2026-09-11 00:06:42 +03:00`, 10 h, so it completes around **10:07 local**.

The rig is a **rover** for the duration: TMODE is off, and the PPS carries
position-solution noise. Timing is degraded but chrony stays locked to PPS2 at stratum 1.

TMODE was dropped in the **RAM layer only**, so a power cut during the survey restores
fixed mode with the old coordinates rather than leaving the rig a rover.

Check progress:

```
gpsstat                 # whole-chain health, exit-coded
f9t-survey status       # fixes, scatter, realistic accuracy
```

Expect the realistic accuracy figure to settle around **0.4 m ≈ 1.3 ns**. Ignore the
"naive standard error" line — it is printed only to show how misleading it is.

---

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

## Step 2 — PPP, for the number nothing on the rig can measure

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

---

## Step 3 — the RTC, whenever convenient

The single outstanding *hardware* fault, and the root of the outage on 2026-09-10.

1. **Fit a backup cell** to the CM4 IO Board's RTC connector. The PCF85063 reports
   `Power loss detected, invalid time`, so every cold boot comes up at the filesystem
   date via `fixrtc`, chrony steps the clock by years, and gpsd silently stops writing
   SHM(0) — taking both GNSS refclocks down with it. `rtcsync` is already in
   `chrony.conf`, so the RTC will be correct as soon as it can hold charge.
2. **Install `fake-hwclock`** — not currently installed. Belt and braces: boot comes up
   at last-shutdown time rather than the filesystem date, so even a flat cell leaves a
   step of hours rather than years.
3. **Re-arm gpsd after any step.** Prefer a oneshot running `systemctl try-restart gpsd`
   after `time-sync.target`. Do *not* simply enable `chrony-wait` and order gpsd after
   it: with the network down at boot, chrony has no source, `chrony-wait` blocks, and
   gpsd — the only remaining time source — never starts.

Until then: **restart gpsd after any cold boot.** `gpsstat` reports the condition as a
`NOTE` on every run so it does not get forgotten.

---

## Step 4 — smaller items, no particular order

- **NMEA `offset` is wrong by ~0.2 s.** With `offset 0.028` the source reads +201 to
  +223 ms. Harmless today (NMEA is `noselect` and only numbers PPS pulses, needing
  ±0.5 s) but it spends half the ambiguity budget. Correct to ≈0.24 — **determine the
  sign empirically**, chrony's display convention is easy to invert — and do it
  deliberately, since it costs a chronyd restart.
- **UART1 at 38400 is the latency.** The serial cycle lands ~250 ms after the second.
  115200 would cut it to ~85 ms. Now less urgent: the seven RTCM3 base-station message
  types were switched off on 2026-09-10, which removed a large part of the load. Do this
  before adding any further constellations.
- **Build the connectors:** 75 Ω terminator, 50 Ω terminator, BNC tee. Still the only
  thing gating the DA/Orion measurement, which now needs the scope carried to the rack.
- **DA output into a real 75 Ω load, and Orion's actual termination** — one procedure,
  three readings (README §7 item 8).
- **Antenna inspection:** where the coax shield is bonded, and a gas-discharge arrestor
  at the entry point if the run is roof-mounted.
- **`ntpsec` leftover** still runs an `ntploggps` cron entry. Guarded by
  `[ ! -d /run/systemd/system ]` so it never fires here, but it is dead weight sitting
  next to chrony.

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

Current state, after the 2026-09-10 changes and with the survey still running.

| Term | Size | Status |
|---|---|---|
| Antenna cable delay | 35 ns | **fixed** — 5 → 40 ns, calculated not measured |
| Stored position, stale since the antenna moved | ~19 ns | survey in progress → ~1.3 ns |
| Antenna LNA group delay | 10–30 ns `[?]` | **uncompensated**, unmeasurable here; PPP |
| Receiver internal delay | unknown `[?]` | **uncompensated**; PPP |
| chrony PPS2 jitter (std dev) | 22 ns | random, averages out |
| Receiver time solution (`tAcc`) | 1 ns | was 3 ns before dual-frequency Galileo |
| Ionospheric residual | few ns | reduced by GPS L1+L2C and Galileo E1+E5b |

Two things worth reading off this table:

**The systematic terms dominate, and most are still unmeasured.** Cable delay, LNA delay
and receiver delay are constant offsets — they do not average out, and no amount of
observation on this rig reveals them. Only PPP does.

**The survey is polish, not repair.** It buys ~1.3 ns against a 35 ns cable fix that cost
one config write. Worth doing, but keep the proportion in mind: the biggest remaining
prize is the LNA and receiver delay pair, and that is Step 2.

---

## Recovery — if something goes wrong

**Survey went wrong, or needs abandoning:**

```
f9t-survey abort        # restores fixed mode with the previous coordinates
```

The previous values are also in `~/f9t-survey/survey.meta` as `old_CFG_TMODE_*`, and the
whole pre-change receiver state is in `config/f9t-config-{ram,flash}.txt` in git.

**Receiver configuration is wrong:** every key in `config/f9t-config-ram.txt` is a
`<key> <value>` line, so a restore is `ubx-apply-config` against a filtered copy of that
file. The dump is complete — 945 keys, RAM and Flash byte-identical at the time it was
taken.

**A power cut during the survey** needs no action: TMODE was changed in the RAM layer
only, so the receiver comes back in fixed mode with the old coordinates. Restart the
survey if wanted.

**GNSS refclocks unreachable after a cold boot:**

```
sudo systemctl restart gpsd
```

That is the standing workaround for the RTC fault until Step 3 is done. `gpsstat` will
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
