# Timing software and GNSS

How time gets from the sky into chrony, NTP and PTP on aika, how the ZED-F9T is
configured, and what the accuracy figures do and don't mean.

## Host

- **Arch Linux ARM** since 2026-09-14, replacing Ubuntu.
- **Kernel `linux-aika-rt` 7.2.6**, PREEMPT_RT, built from the Raspberry Pi Foundation
  fork and packaged locally. PHY timestamping (`bcm_phy_ptp`) depends on two local
  patches, both prepared for netdev: hardware-timestamping ioctls reaching phylib when
  the MAC has no `ndo_hwtstamp` callbacks (a v7.0 regression that breaks PHY PTP on the
  CM4), and an EXTTS fix so reading the PHC no longer eats pending pulse events (~14 % of
  pulses were lost). The kernel and image build trees are outside this repo.
- `ethtool -T eth0` should report hardware transmit/receive with the PHY as timestamp
  source.

## The chain

```
F9T TIME1 ──► PHY extts (/dev/ptp0) ─┬► chrony refclock PPS2 ─┐
                                     │                        │
F9T UART1 ──► gpsd ──► SHM(0) ──► chrony refclock NMEA ───────┤ (numbers the second)
                                     │                        ▼
                                     │               CLOCK_REALTIME ──► chronyd NTP server
                                     │
                                     └► ts2phc ──► PHC ──► ptp4l grandmaster
```

### chrony (`/etc/chrony.conf`)

```
refclock PHC /dev/ptp0:extpps poll 4 precision 1e-9 refid PPS2 lock NMEA maxlockage 60 prefer maxunreach 10800
refclock SHM 0 poll 4 refid NMEA noselect
server time.mikes.fi  iburst minpoll 6 maxpoll 12
server time1.mikes.fi iburst minpoll 6 maxpoll 12
server time2.mikes.fi iburst minpoll 6 maxpoll 12
driftfile /var/lib/chrony/chrony.drift
makestep 1 3
rtcsync
lock_all
```

- **PPS2** supplies the edge. `lock NMEA` means it needs a sample from the NMEA source to
  decide which second a pulse belongs to. The NMEA sample must fall within ±0.5 s of the
  second, or the pulse is numbered to the wrong second
  ([troubleshooting](troubleshooting.md#uart-bandwidth-is-a-correctness-constraint)).
  Keep the lock: it makes a bad NMEA fail *closed*, starving PPS2, rather than open, and
  what failing open looks like is 527 ms off UTC at stratum 1 with a few-ns offset.
- **`maxlockage 60`** (2026-09-21) is how far that lock is allowed to stretch. The default
  of 2 pulses meant any gpsd SHM silence beyond ~2 s took PPS2 down with NMEA — 10 to 35
  times a day, 36 to 113 s each. A stale NMEA sample is cheap here because it only fixes
  *which* second: over 60 s the rubidium-disciplined clock drifts about 1.7 ns. The
  fail-closed property is kept, just with a 60 s fuse instead of a 2 s one.
- **NMEA** is `noselect` and used only for labelling. At 460800 it reads +45 ms or +57 ms
  and steps between the two every ~18 min. The step follows the sign of `UBX-NAV-PVT nano`
  (the receiver's 1 ms clock-bias sawtooth): with `nano` ≥ 0 gpsd times the sample from the
  UBX burst, with `nano` < 0 from the first NMEA sentence. At 115200 the same two modes sat
  at +52 ms and +105 ms. Each step makes `sourcestats` drop its samples; it is cosmetic.
- **MIKES** servers sit ~1 ms from the local clock with ~10 ms error bars. They are a
  sanity check, not a reference.
- `/dev/pps0` exists (gpsd attaches the PPS line discipline to `ttyAMA0`) but carries no
  pulses: DCD is not wired, so `ppstest` only times out. The PPS that matters goes to the
  PHY, not to a UART handshake line. gpsd's startup `unable to read /dev/pps0` error is a
  race against the device appearing, and is harmless.
- Use the drift file (`ref_freq_offset` in `gpsstat`) to judge rubidium frequency.
  `chronyc tracking` rounds to 1×10⁻⁹.

### gpsd

`/etc/default/gpsd`: `DEVICES="/dev/ttyAMA0"`, `GPSD_OPTIONS="--listenany --nowait
--badtime --passive"`. gpsd owns ttyAMA0 exclusively (`TIOCEXCL`), and ser2net is
disabled. `--passive` means gpsd never writes configuration to the receiver; ubxtool
handles that through gpsd.

### PTP

- `ptp4l@eth0.service` runs ptp4l as grandmaster from `/etc/linuxptp/ptp4l.conf`.
  Arch's linuxptp package ships no systemd units, so these are local.
- `ts2phc@eth0.service` runs `ts2phc -f /etc/linuxptp/ts2phc.conf -s generic -c eth0`
  and disciplines the PHC **directly from the same PPS events** the PHY timestamps.
  `-s generic` means a pulse without time of day, so only the sub-second part is
  corrected and the whole seconds (TAI) are left alone. Config:

  ```
  [global]
  leapfile /usr/share/zoneinfo/leap-seconds.list
  ts2phc.pin_index 0
  ts2phc.channel 0
  ts2phc.extts_polarity rising
  [eth0]
  ```

  `leapfile` is not optional: linuxptp's built-in leap-second table expired in June 2025,
  and without the system file ts2phc rejects every sample with `source ts not valid`.
  The `UTC-TAI offset not set in system! Trying to revert to leapfile` line at startup is
  expected — the kernel's TAI offset is 0 because chrony is not configured with
  `leapsectz`, so ts2phc uses the file instead.
- `phc2sys@eth0.service` is **disabled** since 2026-09-26. It used to run
  `phc2sys -s CLOCK_REALTIME -c eth0`, pushing the disciplined system clock into the PHC,
  which put the slow PHC↔system transfer into the PHC's path. Measuring how far each PPS
  timestamp lands from an exact PHC second gave, over 60–90 s:

  | PHC disciplined by | mean | sd | p95 | max |
  |---|---|---|---|---|
  | phc2sys (system → PHC) | +9.6 ns | 137 ns | +92 ns | +980 ns |
  | ts2phc (PPS → PHC) | −0.3 ns | 6.1 ns | +7 ns | +13 ns |

  A PTP client two switches away improved from ±5.8 ms root dispersion to
  +787 ns ±1.8 µs. The system clock is unaffected: chrony still disciplines
  CLOCK_REALTIME from the same events, and each reader of `/dev/ptp0` gets its own
  event queue, so nothing is stolen from chrony. Thanks to @jclark for the suggestion
  (`mkovero/clock#1`).
- **Holdover caveat.** With no GNSS fix there are no pulses, and the PHC then free-runs on
  whatever clocks the PHY rather than on the rubidium-derived 54 MHz that CLOCK_REALTIME
  keeps. phc2sys used to give the PHC that holdover for free. A fallback that starts
  phc2sys when the pulses stop is not built yet.
- `eee-off@eth0.service` runs `ethtool --set-eee eth0 eee off` before ptp4l starts.
  Energy Efficient Ethernet ruins PTP latency, and on 7.2 the `genet.eee=N` kernel option
  is ignored.

### gpsd self-healing

gpsd silently stops feeding SHM(0) after a large clock step
([troubleshooting](troubleshooting.md#dead-rtc--gpsd--chrony)). Two local units cover
this:

- `gpsd-after-timesync.service` runs `systemctl try-restart gpsd` after `time-sync.target`.
- `gpsd-shm-watchdog.timer` checks every minute and restarts gpsd after **two consecutive**
  silent checks, at most once per 10 minutes.

That two-check threshold means short silences never reach it. gpsd goes quiet for 36–113 s
between 10 and 35 times a day, logging `gpsd SHM(0) silent (check 1)` and recovering on its
own. Those used to take PPS2 down as well until `maxlockage` was raised
([troubleshooting](troubleshooting.md#lock-nmea-took-pps2-down-with-every-gpsd-hiccup)).

Do not order gpsd after `chrony-wait` instead. If the network is down at boot, chrony has
no source, `chrony-wait` blocks, and gpsd, the only remaining time source, never starts.

### Monitoring

`tools/gpsstat` (installed as `~/bin/gpsstat`) checks each link in the order it tends to
break: chrony refclock reach, whether gpsd is writing SHM(0), fix and geometry, RF
jamming and spoofing indicators, rubidium frequency, GNSS-vs-rubidium consistency, and
hardware facts (RTC, ser2net, PHC driver). It exits 0 when healthy, 1 when degraded and 2
when GNSS timing is down. `gpsstat -v` also reads the key receiver settings back.

- Use `note()` for known, accepted conditions and `warn()`/`fail()` only for things that
  need action. The dashboard runs it on a schedule, so a permanent non-zero exit would
  hide real failures.
- RF block jamming state is judged against a site baseline (`F9T_RF_BASELINE`, default
  `0:60 1:7`).
- `config/f9t-reminders` holds date-gated reminders, which gpsstat prints as notes.

`tools/clock-dashboard` archives gpsstat output every 5 minutes and publishes the static
page. See [dashboard/README.md](../dashboard/README.md).

## ZED-F9T configuration

| Setting | Value |
|---|---|
| Mode | `CFG-TMODE-MODE=2`, fixed LLA |
| Position | fixed, from PPP on rapid products, written 2026-09-19 and confirmed against final products 2026-09-30 (2.8 mm apart, still `IAR 0.00%`). 0.38 m 3D (1σ, east widened) ≈ 1.28 ns, ITRF20 ellipsoidal. Coordinates are not in this public repo — see [config/README.md](../config/README.md) |
| Time pulse | **TP1** 1 Hz, 50% duty, `USE_LOCKED_TP1=1`, locked-only since 2026-10-03, on the **GPS** time grid (`TIMEGRID_TP1=1`) since the same day. The coax moved from TP2 to TP1 on 2026-10-02; TP2 keeps its old settings, including the Galileo grid, but is no longer wired |
| Cable delay | `CFG-TP-ANT_CABLEDELAY=40` ns (8 m ÷ (0.66 c) = 40.4 ns). Excludes LNA and receiver delay |
| Signals | GPS L1C/A + L2C, Galileo E1 + E5b, BeiDou B1I + B2I |
| UART1 | 460800. RTCM3 base-station output off. RAWX/SFRBX on only while logging for PPP |

- **Position error becomes constant time bias** at about 3.3 ns per metre, because TMODE
  fixed treats the position as truth. `CFG-TMODE-HEIGHT` is height **above the
  ellipsoid**. Entering geoid height here would add an error of ~17 m, about 57 ns.
- **Constellations deliberately left off:** GLONASS (FDMA inter-frequency biases), QZSS
  (not visible at 60°N) and SBAS (adds nothing to a fixed dual-frequency receiver). The
  reasoning is kept in `config/f9t-timing-changes.txt`. BeiDou B2I is enabled but never
  shows up in RINEX, probably a firmware limit, and it has been left alone.
- The per-constellation `CFG-SIGNAL-*_ENA` master switches can read 1 even when every
  signal under them is 0. Check the individual signal keys.
- **Known-good configuration.** `config/f9t-known-good.txt` holds the timing-critical keys
  (time pulses, cable delay, fixed position, signals, RTCM off) as read back from a healthy
  receiver. `f9t-restore --check` compares the receiver with it, `f9t-restore` writes back
  whatever differs in RAM, and `f9t-restore --persist` also fixes Flash. On aika
  `f9t-restore.service` does the RAM restore once per boot after gpsd, and `sudo systemctl
  start f9t-restore` does it on demand. gpsd-managed message outputs and the UART1 baud rate
  are left out on purpose: gpsd rewrites the former in RAM, and changing the latter mid-run
  cuts the link.

### Files in `config/`

| File | What |
|---|---|
| `f9t-timing-changes.txt` | changes applied since then, with reasons (`ubx-apply-config … 7`) |
| `f9t-reminders` | date-gated reminders shown by `gpsstat` |
| — | the PPP keyfile, `f9t-known-good.txt`, the survey and PPP reports and the full CFG dumps all carry the position, so they live in the private `mkovero/sys` repo instead ([config/README.md](../config/README.md)) |
| `chrony.conf` | copy of `/etc/chrony.conf`, tracked since 2026-09-21 |
| `ar40a-trim-log.txt` | every move of the AR-40A trimmer, with direction, sensitivity and travel used |

## Bias and error budget

**chrony cannot see a constant bias.** It measures PPS against the system clock and then
steers the system clock to the PPS, so any fixed delay is absorbed and `sourcestats`
reports `Offset ~0ns` no matter what. What remains visible:

- **chrony std dev** is jitter.
- **`tAcc`** is the receiver's own estimate.
- **Max-error bound** is root dispersion + delay/2, chrony's guarantee for the system
  clock.

None of these measures absolute UTC bias. **PPP does not measure it either.** PPP solves
for the F9T's free-running TCXO clock, while the time pulse is corrected against the
receiver's own solution, so antenna and receiver delays do not show up in the result. Measuring them
takes a calibrated counter against a second reference, or common-view comparison with a
laboratory.

| Term | Size | Status |
|---|---|---|
| Antenna cable delay | 35 ns | corrected (5 → 40 ns), calculated not measured: 5 m moulded RG-174 + 3 m extension at 5.05 ns/m ≈ 40.4 ns |
| Stored position | 1.28 ns | PPP on rapid products, 2026-09-19; **re-run against final products 2026-09-30 moved it 2.8 mm (0.01 ns) with identical sigmas, so the stored values stand** (was ~19 ns after the antenna move) |
| Antenna LNA group delay | est. 10–30 ns | uncompensated, not measurable on the rig. **u-blox publishes no group delay for the ANN-MB**, so this is a typical SAW-filtered-antenna figure, not a spec ([hardware](hardware.md)) |
| Receiver internal delay | unknown | uncompensated, not measurable on the rig |
| chrony PPS2 jitter | 20–60 ns | random, averages out |
| Receiver `tAcc` | 1 ns | — |
| Ionospheric residual | few ns | reduced by dual-frequency GPS, Galileo and BeiDou |

Fixed offsets are the dominant terms, and the largest of them are still unmeasured.

### The F9T's own oscillator (byproduct of PPP, 2026-09-30)

The clock series in a CSRS-PPP result is the receiver's **free-running TCXO** against GPS/IGS
time — nothing steers it, and it is not the 1 PPS. `tools/ppp-clk-adev` turns that into an
Allan deviation, which comes free with any PPP run:

```
tools/ppp-clk-adev aika-20260912-0942.clk
```

From the 23.2 h final-products run (2768 epochs at 30 s):

| τ | ADEV |
|---|---|
| 30 s | 1.2×10⁻⁹ |
| 2 min | 2.0×10⁻⁹ |
| 8 min | 3.2×10⁻⁹ |
| 1.1 h | 4.5×10⁻⁹ |
| 4.3 h | 9.5×10⁻⁹ |

Rising with τ, i.e. dominated by drift and temperature rather than noise, which is what a
TCXO does. For scale the AR-40A reads 5.7×10⁻¹² at 1 h, about 800× better, and it is the
rubidium — through the 54 MHz into the CM4 — that carries aika's frequency. The F9T's crystal
only has to be good enough to hold the receiver's own solution together between fixes.

`--json out.json` writes the same table for the dashboard: `clock-dashboard --tcxo-adev` picks it
up (default `~/.local/state/clock-dashboard/tcxo-adev.json`) and publishes it as `tcxo_adev`,
which the page shows as a "receiver oscillator stability" card with the measurement date. The
collector never computes it — it needs a CSRS-PPP clock file — so the card simply stays hidden
until a PPP run produces one.

**Two things to know before trusting such a series:**

- **The receiver steps its clock.** Two jumps in 23 h, −25.986 ms and −23.986 ms, at roughly
  15 h intervals: the crystal runs **+0.456 ppm** fast, drift accumulates ~25 ms, and the
  receiver puts it back. `ppp-clk-adev` removes and reports the steps; leaving them in gives
  ~2×10⁻⁵ at every τ, which is what the jumps measure, not the oscillator.
- **This says nothing about the time pulse.** It is steered against the receiver's own solution
  and never appears in the PPP clock.

### Allan deviation on the dashboard

`clock-dashboard` computes the AR-40A's overlapping ADEV from its own history on every run and
publishes it as `ref_adev`; the page shows it as a "reference oscillator stability" card. Method
and traps are those of `tools/rb-adev`: chrony writes the drift file hourly, so the series is
bucketed to one value per UTC hour and tau starts at 1 h, and the overlapping estimator
differences m-point averages separated by m.

It uses the longest gap-free run inside the **last 7 days**, which matters more than it sounds.
The same series over the 19 days to 2026-09-30 gives 2.8×10⁻¹⁰ at 1 h, because a trim, several
kernel swaps and power cycles fall inside it; the last 48 h give 8.1×10⁻¹², matching the quiet
22 h window measured on 2026-09-22 (5.7×10⁻¹²). A frequency step is a real event, and ADEV
cannot tell it from noise — so read the card as "how the rig has behaved lately", and expect
maintenance to inflate it for days.

### What the frequency readout can actually resolve

Measured 2026-09-22 over 22 h of undisturbed `ref_freq_offset` (2026-09-20 11:00 to
2026-09-21 09:00 UTC, after the trim had settled). Overlapping Allan deviation:

| τ | ADEV |
|---|---|
| 1 h | 5.7×10⁻¹² |
| 2 h | 4.1×10⁻¹² |
| 4 h | 3.8×10⁻¹² |
| 8 h | 2.8×10⁻¹² |

**The floor is a few ×10⁻¹² over hours**, and it agrees with the independent check: the
hourly means wandered ±8×10⁻¹² across that night.

Three caveats, all of which make this a measure of the *readout*, not of the AR-40A:

- The drift file is written **hourly**, so 5-minute sampling re-reads one value about
  twelve times. Points below τ = 1 h are not independent and mean nothing.
- The series is chrony's servo-filtered frequency estimate, not a raw phase comparison
  against the PPS, so it is smoothed by the loop before anyone sees it.
- Systematic terms from the table above — position, LNA and receiver delay — do not
  average down at all. This is repeatability, not accuracy.

> **Computing this correctly matters.** Overlapping Allan deviation differences m-point
> averages **separated by m**. Differencing *adjacent* sliding averages, which share m−1
> of their m points, makes ADEV fall as τ⁻¹ and produced a figure 60× too good before the
> ±8×10⁻¹² overnight wander contradicted it.

Consequences:

- The AR-40A at 2.2×10⁻¹¹ sits about 7× above the floor, so it is genuinely measurable.
  The 2026-09-20/21 trim stopped at 2.8×10⁻¹¹, which was 0.90× chrony's own skew — that
  was the right place to stop, and this is why.
- **Anything better than ~1×10⁻¹¹ is not resolvable on this rig.** A 5071A-class caesium
  at ±5×10⁻¹³ would be below both this floor and the drift file's own 1×10⁻¹² print
  resolution — invisible twice over.
- **Do not read the AR-40A's trimmed offset as comparable to a caesium.** It reads
  2.2×10⁻¹¹ because it was trimmed against GNSS on 2026-09-21, and that number is
  borrowed rather than owned. Aging at 5×10⁻¹⁰/yr is 1.4×10⁻¹² per day, so it passes
  1×10⁻¹¹ in about a week and returns to today's figure in sixteen days; the ±2×10⁻¹⁰
  temperature spec is nine times the whole trim residual. A caesium is a primary
  standard: accurate by reference to the hyperfine transition that defines the second,
  with no calibration and no expiry. The one fair comparison is the AR-40A's <3×10⁻¹²
  ADEV at 100 s, which is respectable at that τ — but that is stability, not accuracy,
  and caesium keeps improving with τ where rubidium walks off.
- That is not a display problem to be fixed with more digits. At those levels the GNSS
  link is the limit: half the sky is blocked
  ([hardware](hardware.md#what-the-antenna-can-actually-see)), the stored position is good
  to 1.28 ns, and the antenna and receiver delays are unmeasured. A better reference would
  have to be compared some other way — common-view against a laboratory, or a counter
  against a second standard.

## Corrections: DGNSS no, PPP for position

**DGNSS (Maanmittauslaitos FinnPos, RTCM MSM1, ~0.5 m) does not belong in the running
configuration.**

- A receiver in TMODE fixed is a base station and ignores incoming corrections.
- If corrections were applied, they would make timing worse. A code correction contains
  the reference station's receiver clock offset. For positioning that offset is absorbed
  harmlessly, but for timing it would tie the rig to a FinnRef station's clock instead of
  UTC.
- DGNSS is only useful during a survey, where the rover's clock is thrown away anyway. In
  practice it did not work even then: gpsd's relay drops RTCM 1006/1008, so the receiver
  never formed a differential solution.

gpsd quirks found along the way, encoded in `tools/ntrip-relay`:

- Its `ntrip://` client rejects the `RTCM3.2` format string.
- A `tcp://` source is never relayed.
- `dgpsip://` drops the port number.
- Writing RTCM directly to the serial port fails with `EBUSY` because gpsd holds the port
  exclusively.

Restructuring so the relay owns the port is not worth it for a 0.5 m service.

### Comparing against UTC(MIKE)

MIKES submits **PPP time-transfer files to the BIPM** and they are public:
`https://webtai.bipm.org/ftp/pub/tai/data/<year>/time_transfer/ppp/mikeYYMM.gpi`. CGGTTS-style
header, `REF = UTC(MIKE)`, a `Cal_Id` (their station is calibrated), their ITRF position and
their delays (`INT DLY`, `CAB DLY 215.4 ns`, `REF DLY 8.6 ns`), then 5-minute rows of
`REFGPS` = UTC(MIKE) − GPS time in ns, tagged by MJD. Files appear monthly and cover ~34 days,
so a September run needs `mike2609.gpi`, published in the following weeks.

`tools/ppp-vs-utcmike` downloads that file and differences it against the per-epoch receiver
clock in a CSRS-PPP `.pos` (from the **full** result archive, not the `.sum`):

```
tools/ppp-vs-utcmike --fetch 2609 --ppp full_output.pos
```

**As it stands this comparison is not usable, and the 2026-09-30 finals run shows why.** The
clock CSRS-PPP reports is the F9T's free-running TCXO: +589 us at the start, -11222 us 23 h
later, drifting -0.141 ppm. MIKES's REFGPS is tens of ns. Differencing them measures our
crystal. The time pulse is corrected against the receiver's own solution and never appears in the PPP
clock, as the error-budget section above already noted.

The missing link is the receiver's own tie between its clock and the pulse:

```
PPS - GPS time = (PPS - receiver clock, from UBX-TIM-TP) - (receiver clock - GPS time, from PPP)
```

So a RAWX session intended for time transfer must log **UBX-TIM-TP** as well.

> **Superseded 2026-10-02 by rewiring.** Everything in this subsection was measured while the
> PHY's pulse came from TP2. The coax now runs from **TP1**, so `UBX-TIM-TP`'s `qErr` describes
> the pulse that feeds the PHY, and the question below — whether TP1's qErr can stand in for
> TP2's edge — no longer needs an answer. The measurements are kept because they are what made
> the case for rewiring, and because the 1.5× slope remains unexplained.

**Measured 2026-09-30: TIM-TP describes TP1, not TP2.** Enabling `CFG-MSGOUT-UBX_TIM_TP_UART1`
in RAM gives one message per second with `qErr` in picoseconds (±3.5 ns sawtooth observed),
`flags (timebase:GNSS UTC:OK RAIM:active qErr:Valid TP:Locked)` and `refInfo (GNSS:Galileo)`.
Setting TP1's period to 2 s left the cadence at 1 Hz (inconclusive), but **disabling TP1
stopped TIM-TP entirely**, so the message follows TP1. TP2 — the pulse that feeds the PHY and
therefore the whole timing chain — has no quantisation-error reporting. Both were restored
afterwards; PPS2 stayed selected at stratum 1 throughout.

**Can TP1's qErr stand in for TP2? Tested 2026-09-30: no.** `tools/qerr-vs-extts` logs TIM-TP
(TP1's qErr) and the PHY's own EXTTS timestamps of TP2 at the same time, pairing them by GPS
time of week. Over 236 pulses:

| lag | pairs | correlation | sd(EXTTS) | sd(EXTTS − qErr) |
|---|---|---|---|---|
| −1 s | 236 | +0.222 | 6.20 ns | 6.10 ns |
| 0 | 237 | −0.104 | 6.21 ns | 6.79 ns |
| +1 s | 237 | −0.063 | 6.27 ns | 6.77 ns |

Raw correlation is weak because slow wander dominates. **First-differencing** the series kills
that wander and leaves the per-second sawtooth, which is where the answer is. Over 283
consecutive pairs (ts2phc running; differencing makes the servo irrelevant):

```
slope d(extts)/d(qErr) = -1.556 +/- 0.162      correlation -0.497
sd d(extts) 8.84 ns   sd d(qErr) 2.82 ns
```

So TP1's qErr **does** carry real information about TP2's edge — 9.6 sigma from zero — but the
slope is 3.4 sigma away from the −1.000 that a shared quantisation would give. The pulses are
related, not identical. Correcting TP2 with a fitted −1.56 × qErr removes about 25 % of the
variance (13 % of the scatter); a clean 1:1 correction is not available.

Conclusion: **rewiring the PHY's pulse to TP1 remains the right fix** if qErr matters, and
re-running this tool afterwards is its acceptance test — the slope should then come out at −1.
The empirical 1.5× factor is a curiosity, possibly different rounding granularity in the two
pulse generators; it is not understood.

**Done 2026-10-02, and the acceptance test failed.** The coax moved to TP1 at the bench, and
the three keys that make TP1 behave as TP2 did were written on 2026-10-03: `DUTY_LOCK_TP1 50`,
then `USE_LOCKED_TP1 1`, then `DUTY_TP1 0`. That order matters — `DUTY_LOCK_TP1` starts at 0, so
switching first would point the receiver at an empty locked set and stop the pulse.

Re-running `tools/qerr-vs-extts` on the new wiring, 300 s with ts2phc running, 286 consecutive-
second pairs:

```
slope d(extts)/d(qErr) = -1.469 +/- 0.133      correlation -0.549
sd d(extts) 8.39 ns   sd d(qErr) 3.13 ns
3.5 sigma from -1.000,  11.1 sigma from 0
```

Statistically unchanged from the −1.556 ± 0.162 measured across two different outputs — which
looked at first like the factor being intrinsic. It is not. **It was the servo.**

Repeating the run with **ts2phc stopped**, same length, same 286 consecutive-second pairs:

| ts2phc | slope d(extts)/d(qErr) | from −1.000 | from 0 | r | sd d(extts) | sd d(qErr) |
|---|---|---|---|---|---|---|
| running | −1.469 ± 0.133 | 3.5σ | 11.1σ | −0.549 | 8.39 ns | 3.13 ns |
| **stopped** | **−0.955 ± 0.209** | **0.2σ** | 4.6σ | −0.261 | 12.47 ns | 3.41 ns |

Servo-free the slope is exactly what a shared quantisation predicts, and still 4.6σ from zero,
so the relationship is real. **`qErr` does describe the wired pulse's quantisation 1:1.** The
two slopes differ by 2.1σ, which is suggestive rather than decisive on its own, but the physical
story is plain: a servo that steers the clock from the same timestamps it is being measured
against will bias the regression, and the claim elsewhere in this file that first-differencing
makes the servo irrelevant is simply wrong.

That also undermines the −1.556 result above, which was taken with ts2phc running. The
conclusion drawn from it — that TP1 and TP2 are "related, not identical" — is not supported;
most or all of that departure from −1 was probably the same artefact rather than anything about
the two outputs.

The servo-free run costs what the earlier attempt said it costs: the PHC free-runs and wanders
about 400 ns over five minutes, which swamps the undifferenced correlation, and the downstream
PTP client follows a drifting master for the duration. Everything recovered within a minute of
restarting ts2phc (PPS2 +3 ns, PHC −1 ns, the PTP client back to +462 ns).

A free-running variant (ts2phc stopped, the tool holding the EXTTS channel open itself via
`--enable-extts`, since stopping ts2phc otherwise disables the channel and starves chrony too)
gave the same picture with far worse SNR: the PHY's oscillator wanders ~95 ns over four minutes,
differenced correlation −0.36. Not worth the disturbance: it also cost the downstream PTP client
its lock.

Consequences:

- **The qErr correction is sound in principle, and worth single-digit percent in practice.**
  TIM-TP describes the wired pulse and the servo-free slope is 1:1, so the information is
  genuinely there. But the quantisation is a minority of the noise: differenced, `qErr` carries
  3.1 ns against the PHY timestamps' 8.4 ns, so even a perfect correction takes the scatter from
  8.39 to about 7.8 ns — roughly 7 %. Worth having, not transformative, and it needs code that
  does not exist: `ts2phc` has no `qErr` input, so realising it means patching linuxptp or
  moving to something like SatPulse. Note also that subtracting `qErr` from the *servo-running*
  series makes things worse (7.61 → 8.59 ns), because the servo has already absorbed part of it
  — a correction has to go inside the loop, not after it.
- **The wired pulse moved to the GPS grid on 2026-10-03** (`CFG-TP-TIMEGRID_TP1=1`,
  `refInfo` low nibble 3 → 0), precisely so that a comparison against a GPS-time reference such
  as the BIPM's `REFGPS` does not carry the Galileo-to-GPS offset. TP2, the unwired spare, stays
  on grid 4. The switch was itself a measurement — see below.
- `tools/ppp-vs-utcmike` is **unblocked in principle**: the tie between the receiver clock and
  the pulse now exists, because TIM-TP describes the wired output, and the GGTO term has been
  removed by moving to the GPS grid. It still needs a RAWX session logging `UBX-TIM-TP`
  alongside, and a MIKES month that overlaps it — the BIPM publishes `mikeYYMM.gpi` a couple of
  weeks after month end, so an October session yields a number in mid-November. That publication
  date is the binding constraint; CSRS-PPP returns rapid products in about a day and finals in
  about two weeks.

### Measuring the GGTO by switching the grid

Changing `CFG-TP-TIMEGRID_TP1` from 4 to 1 at 2026-10-03T00:18:36Z steps the pulse by exactly
the Galileo-to-GPS offset, so the change is also the measurement. From chrony's
`refclocks.log`, PPS2 raw offsets either side:

| grid | samples | mean | sd |
|---|---|---|---|
| 4 = Galileo | 416 | −9.47 ns | 20.59 ns |
| 1 = GPS | 106 | +1.34 ns | 13.98 ns |

**Step = +10.81 ± 1.69 ns (6.4σ)** — the GGTO as this receiver realises it, consistent with the
few-nanosecond figure the term is usually quoted at. `UBX-TIM-TP`'s `refInfo` went from `x03` to
`x00` (low nibble 3 = Galileo → 0 = GPS), confirming the pulse followed rather than just the
setting. The system clock never moved more than a nanosecond; chrony absorbed the step.

Worth noting what this is not: it is one receiver's view over a few minutes, not a GGTO
determination. It is quoted here because it is the size of the systematic that would otherwise
have sat silently inside any comparison against a GPS-time reference.

**CGGTTS was tried first and parked** (2026-09-29). `rnx2cggtts` 1.0.2 builds (pin `time` to
0.3.41 first) and writes a valid CGGTTS header from our RINEX, but produces **zero tracks**:
it rejects RINEX v3 clock products as "invalid file type", and fails candidate formation with
"at least one pseudo range observation is mandatory" on our GPS observables (the F9T logs
`C1C` with L2C as `C2L`/`C2S`). Since MIKES submits PPP rather than common-view tracks anyway,
the PPP route above is the direct comparison; CGGTTS would need a solver config or a patched
tool. Reported upstream as
[nav-solutions/rinex#450](https://github.com/nav-solutions/rinex/issues/450) (the project moved
from `georust` to `nav-solutions`).

**PPP** (RAWX → RINEX → NRCan CSRS-PPP) gave the stored position, using precise orbit and
clock products with no dependence on another receiver's clock. The rapid-product rerun
of 2026-09-19 is the one in Flash. It is still a float solution, and the reason is the
antenna's sky, not the products: half the sky returns no carrier phase at all
([hardware](hardware.md#what-the-antenna-can-actually-see)). `tools/f9t-skymap` measures
this from any RAWX log and is the way to tell whether moving the antenna helped.

## Tools

Installed by copying into `~/bin` on aika. `F9T_TARGET` / `UBXOPTS` name the gpsd device
for ubxtool.

| Tool | Purpose |
|---|---|
| `gpsstat` | one-screen health check of the whole chain, exit code 0/1/2 |
| `clock-dashboard` | archive gpsstat snapshots, generate dashboard `data.json` |
| `publish-clock-dashboard`, `clock-dashboard-askpass` | atomic SFTP publish of the dashboard |
| `ubx-dump-config` | full CFG dump, RAM and Flash, paged |
| `ubx-apply-config` | apply a key/value file and verify each write by reading it back |
| `f9t-restore`, `f9t-restore.service` | compare the receiver with `f9t-known-good.txt` and write back only what differs: `--check`, RAM (default, also once per boot), `--persist` (RAM + BBR + Flash) |
| `f9t-rawlog` | log RAWX/SFRBX for PPP without leaving fixed mode; checks UART margin first |
| `f9t-ppp` | RAWX → RINEX via `convbin` for PPP submission |
| `f9t-survey` | standalone position survey (rover mode), then write the result into TMODE |
| `f9t-skymap` | C/N0 and carrier-phase yield by azimuth and elevation from a RAWX log — what the antenna can see |
| `rb-freq-live` | AR-40A offset live from `adjtimex` at 1.5×10⁻¹¹, for turning the trimmer against a moving number |
| `rb-adev` | overlapping Allan deviation of the frequency series — what the rig can resolve |
| `ntrip-relay` | NTRIP client and local re-caster. Not used in steady state |
