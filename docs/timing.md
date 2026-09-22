# Timing software and GNSS

How time gets from the sky into chrony, NTP and PTP on aika, how the ZED-F9T is
configured, and what the accuracy figures do and don't mean.

## Host

- **Arch Linux ARM** since 2026-09-14, replacing Ubuntu.
- **Kernel `linux-aika-rt` 7.2.5**, PREEMPT_RT, built from the Raspberry Pi Foundation
  fork and packaged locally with a bcmgenet hardware-timestamping patch. PHY timestamping
  (`bcm_phy_ptp`) depends on that kernel. The kernel and image build trees are outside
  this repo.
- `ethtool -T eth0` should report hardware transmit/receive with the PHY as timestamp
  source.

## The chain

```
F9T TIME2 ──► PHY extts (/dev/ptp0) ──► chrony refclock PPS2 ─┐
F9T UART1 ──► gpsd ──► SHM(0) ──► chrony refclock NMEA ───────┤ (numbers the second)
                                                              ▼
                                                     CLOCK_REALTIME ──► chronyd NTP server
                                                              │
                                              phc2sys ──► PHC ──► ptp4l grandmaster
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
- There is no `/dev/pps0`. gpsd logs `unable to read /dev/pps0` on startup, and that
  message is harmless.
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
- `phc2sys@eth0.service` runs `phc2sys -s CLOCK_REALTIME -c eth0` to push the
  disciplined system clock into the PHC.
- `eee-off@eth0.service` runs `ethtool --set-eee eth0 eee off` before ptp4l starts.
  Energy Efficient Ethernet ruins PTP latency, and on 7.2 the `genet.eee=N` kernel option
  is ignored.

### gpsd self-healing

gpsd silently stops feeding SHM(0) after a large clock step
([troubleshooting](troubleshooting.md#dead-rtc--gpsd--chrony)). Two local units cover
this:

- `gpsd-after-timesync.service` runs `systemctl try-restart gpsd` after `time-sync.target`.
- `gpsd-shm-watchdog.timer` checks every minute and restarts gpsd if SHM(0) has stopped.

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
| Position | 60.179598658°, 24.958725025°, 22.0279 m ellipsoidal (ITRF20). PPP on rapid products, written 2026-09-19, 0.38 m 3D (1σ, east widened) ≈ 1.28 ns. Keys in `config/f9t-ppp-position.txt` and `config/f9t-known-good.txt` |
| Time pulse | TP2 1 Hz, 50% duty, `USE_LOCKED_TP2=1`, locked-only since 2026-09-14 |
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
| `f9t-config-ram.txt`, `f9t-config-flash.txt` | full CFG dump from 2026-09-10, taken **before** the timing changes. 945 keys, restorable |
| `f9t-timing-changes.txt` | changes applied since then, with reasons (`ubx-apply-config … 7`) |
| `f9t-known-good.txt` | timing-critical keys read back from a healthy receiver on 2026-09-15 (RAM == Flash); used by `f9t-restore`. Copied to `~/f9tcfg/` on aika |
| `survey-2026-09-11.meta` | standalone survey result, superseded by PPP |
| `ppp-2026-09-12.sum`, `ppp-2026-09-12-rapid.sum`, `f9t-ppp-position.txt` | CSRS-PPP reports (ultra-rapid, then rapid) and the TMODE keys written from the rapid one |
| `f9t-reminders` | date-gated reminders shown by `gpsstat` |
| `chrony.conf` | copy of `/etc/chrony.conf`, tracked since 2026-09-21 |
| `rb-freq-live` | AR-40A frequency offset live from `adjtimex`, for tuning the trimmer |
| `rb-adev` | overlapping Allan deviation of the frequency series — what the rig can resolve |

## Bias and error budget

**chrony cannot see a constant bias.** It measures PPS against the system clock and then
steers the system clock to the PPS, so any fixed delay is absorbed and `sourcestats`
reports `Offset ~0ns` no matter what. What remains visible:

- **chrony std dev** is jitter.
- **`tAcc`** is the receiver's own estimate.
- **Max-error bound** is root dispersion + delay/2, chrony's guarantee for the system
  clock.

None of these measures absolute UTC bias. **PPP does not measure it either.** PPP solves
for the F9T's free-running TCXO clock, while TP2 is corrected against the receiver's own
solution, so antenna and receiver delays do not show up in the result. Measuring them
takes a calibrated counter against a second reference, or common-view comparison with a
laboratory.

| Term | Size | Status |
|---|---|---|
| Antenna cable delay | 35 ns | corrected (5 → 40 ns), calculated not measured |
| Stored position | 1.28 ns | PPP on rapid products, 2026-09-19 (was ~19 ns after the antenna move) |
| Antenna LNA group delay | typically 10–30 ns | uncompensated, not measurable on the rig |
| Receiver internal delay | unknown | uncompensated, not measurable on the rig |
| chrony PPS2 jitter | 20–60 ns | random, averages out |
| Receiver `tAcc` | 1 ns | — |
| Ionospheric residual | few ns | reduced by dual-frequency GPS, Galileo and BeiDou |

Fixed offsets are the dominant terms, and the largest of them are still unmeasured.

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
- **Anything better than ~1×10⁻¹¹ is not resolvable on this rig.** An HP 5061A-class
  caesium (~1×10⁻¹¹) would sit where the rubidium is now; a 5071A (±5×10⁻¹³) would be
  below both this floor and the drift file's own 1×10⁻¹² print resolution.
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
| `ntrip-relay` | NTRIP client and local re-caster. Not used in steady state |
