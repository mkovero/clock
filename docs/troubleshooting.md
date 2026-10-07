# Troubleshooting and known traps

Failures that have already happened on this rig, how they showed up, and the shortcut
to recognising them.

## Quick reference

| Symptom | Likely cause | Action |
|---|---|---|
| CM4 power and green LEDs lit, no activity flicker | no valid 54 MHz at XIN | check the panel REF LED and Teensy status. See [no reference](#no-reference-at-the-si5351) |
| `rpiboot` enumerates the eMMC | nothing wrong with the clock: the boot ROM runs off XIN, so the whole clock chain is good | look elsewhere |
| `eth0 failed to connect PHY` or any other kernel message | the CM4 **did** boot | see [PHY missing after a warm reboot](#phy-missing-after-a-warm-reboot) |
| CM4 always comes up in USB boot mode | J2 1–2 (`nRPI_BOOT`) jumper left on | remove it |
| `chronyc tracking` looks healthy at stratum 3, refclocks `Reach 0` | gpsd has stopped writing SHM(0) | `sudo systemctl restart gpsd`. See [dead RTC](#dead-rtc--gpsd--chrony) |
| Stratum 1 with tiny offset, but ~0.5 s off UTC (MIKES disagrees) | NMEA arrives too late, so pulses are numbered to the wrong second | reduce UART1 load. See [UART bandwidth](#uart-bandwidth-is-a-correctness-constraint) |
| ubxtool readbacks come back empty, `No path specified in DEVICE` | gpsd has more than one device attached | pass `localhost:2947:/dev/ttyAMA0` (tools use `$F9T_TARGET`) |
| F9T silent on UART after rewiring the PPS | PPS coax shield bonded at both ends | lift the shield at the F9T end |
| Arduino build: wall of `redefinition of …` | stray second `.ino` in the sketch folder, e.g. `foo.ino.ino` | delete it. The primary sketch must match the folder name |

**Restore the timing configuration** with `f9t-restore --check` to see what differs, then
`f9t-restore` (RAM) or `f9t-restore --persist` (RAM + Flash); it only writes keys that differ.
**Restore the full receiver configuration** from the dump with `ubx-apply-config` on a filtered
copy of `config/f9t-config-ram.txt` (every line is `<key> <value>`), then re-apply
`config/f9t-timing-changes.txt` and `config/f9t-ppp-position.txt`.

## PHY missing after a warm reboot

*2026-09-15, updated 2026-09-26.* After a reboot the CM4 can come up with no Ethernet at all:

```
mdio_bus unimac-mdio--19: MDIO device at address 0 is missing.
could not attach to PHY
bcmgenet fd580000.ethernet eth0: failed to connect to PHY
```

The rest of the system boots normally; there is simply no network, and with no console login the box is
unreachable. Observed on several kernels, so it is not a kernel regression.

- **The PHY is not dead: it answers at MDIO address 2 instead of 0.** A debug scan of all 32 addresses on a
  failing boot finds `PHYSID=0x600d84a2` at address 2 and nothing at 0, on normal and `sysrq-b` reboots alike.
  A scan taken at the same early stage on a good boot finds address 0 only. Raspberry Pi confirmed in
  [raspberrypi/linux#5497](https://github.com/raspberrypi/linux/issues/5497) that the PHY's LED pins double as
  address straps while it resets, so a reset that samples the wrong level lands on the wrong address.
- **Mitigation in `config.txt`: `dtparam=eth_led1=4`** turns the CM4's yellow Ethernet LED (PHY LED3) off. With it
  driven as a link LED every warm reboot failed (0 of 12); with it off, 8 of 11 survived.
- **What decides the rest is the moment of the reset within the UTC second**: with LED3 off, resets in one half of
  the second come back at address 2 and in the other half at address 0 (20 of 20 either way, `sysrq-b` fired at a
  chosen phase). Disabling the F9T's pulse entirely does not change that — tested with TP2, which fed the PHY until
  2026-10-02 — so the PPS on `SYNC_OUT` is not the
  trigger; what marks the second is still unidentified. Planned reboots timed to the good half have not failed.
- **Recovery:** switch the **whole 5 V supply** off for ~30 s (leave the rubidium's 15 V on), then on. A power cycle
  of the CM4 alone is not reliable, and neither is another reboot. A debug kernel patch that follows the PHY to
  whichever address answers restores the network on a failed boot and has been used for remote testing.
- **Ruled out:** the `BMCR_PDOWN` write at shutdown (`sysrq-b` skips it and still fails), back-powering of the CM4
  from the other 5 V devices (3V3 measures 28 mV with the CM4's own feed removed), and the F9T PPS itself.
  `brcm,powerdown-enable` in the device tree was cleared with too few reboots to count and is open again.

Plan reboots of the timing host for when someone can power-cycle the rig, unless a kernel with the
follow-the-address patch is running.

## Several unrelated signals die at once → reseat the CM4

Symptom, 2026-10-02 after moving the rig to a new enclosure: no EXTTS events on `/dev/ptp0` and
no data from the Teensy on `/dev/ttyAMA5`, while Ethernet, eMMC, the I²C RTC and the F9T's UART
all worked normally.

Both signals were chased separately and both looked impossible. The pulse was present at the
TP1 pad, the cable measured end to end, pulses reached 3.2 V at the CM4 end, the J2 connection
had never been unplugged, and on the software side the PTP pin was already assigned
`func=extts chan=0`, re-assigning it changed nothing, the channel enabled cleanly, hardware
timestamping was on, and it was the same kernel build that had logged PPS2 at 11 ns sd minutes
before the shutdown. The Teensy was equally blameless: heartbeat LED blinking, panel LEDs
correct, wiring verified pin by pin, continuity good, ground solid.

The answer was in the pin numbers. `Ethernet_SYNC_OUT` is CM4 pin 18 and `GPIO13`/`RXD5` is
pin 28 — **ten pins apart on the same 100-pin mezzanine connector**. The module had been
disturbed during the rebuild and was not fully seated. Reseating it fixed both at once.

The trap worth remembering: **tightening the screws is not the same as seating the connectors.**
The CM4 mates through two DF40 connectors that need a firm, even push to click home. A module
sitting slightly proud on one side will be held there quite happily by its screws, and power,
eMMC and the Ethernet pairs will all keep working while a handful of contacts in one region
never mate. Unscrew it, lift it clear, press both connectors home, then fit the screws.

So: when two or more signals that share a connector fail together and each one individually
looks impossible, stop chasing them separately and check the seating.

## ts2phc and ptp4l fail at boot, then recover

`ts2phc` logs `failed to open clock` and `ptp4l` logs `ioctl SIOCETHTOOL failed: Invalid
argument`, both a few seconds into boot; `Restart=always` then picks them up and everything
works. Harmless in effect, but every cold boot logs failures and the first seconds run with the
PHC undisciplined.

Cause: `After=sys-subsystem-net-devices-eth0.device` is not the right precondition. The netdev
exists well before `bcmgenet` registers the PHC and enables hardware timestamping — measured on
this box, the device unit released at 5.3 s while genet finished at 5.78 s and the link came up
at 8.87 s.

Fix: an `ExecStartPre` that waits for the *clock*, not the interface. Two things need checking
because they fail separately — the hardware timestamping capability, which is what SIOCETHTOOL
rejects, and the `/dev/ptpN` device, which is what ts2phc opens. Watch the field name: ethtool
before 6.x prints `PTP Hardware Clock: N`, while 7.x prints `Hardware timestamp provider
index: N`. A wait script matching only one of them silently never succeeds, which turns a
cosmetic boot warning into a dead timing chain.

## The RF interference baselines moved after a rebuild

Noticed 2026-10-03, the day after the rig moved into a new rack enclosure, while **the case was
still open** — so the table below is open-case figures, taken before whatever shielding the
finished build provides was in place.

**The case was closed at 2026-10-07T00:28Z, and that settled it.** Closing moves these numbers
upward if the source is inside the box and downward if it is outside. From 237 samples over the
first ~20 h with the lid on, against 1124 samples from the open-case period of 10-03..10-06:

| block | jamInd open | jamInd closed | AGC | noise |
|---|---|---|---|---|
| 0 (L1) | median 31, sd 1.2 | **median 18**, sd 2.3 | 5967 → 5967 | 68 → 67 |
| 1 (L2) | median 24, sd 0.7 | **median 21**, sd 0.6 | 5616 → 5616 | 48 → 47 |

**Both fell and neither AGC moved, so the interferer is outside the box** and the enclosure shields
against it. Block 0 dropped 13 points, block 1 three. That **retires the 10 MHz-harmonic hypothesis
below as the leading suspect** — a source inside the case, sharing it with the antenna feed, should
have got worse when the lid went on, not better. The spectrum hunt (`sys/aika2arch/bench-measure-20261006.md`
§E) is therefore parked rather than pending.

`tools/gpsstat`'s site baselines were re-derived from this data at the same time: `0:18 1:21` with
the tolerance tightened from ±15 to ±8. The old `0:60 1:7` was from 2026-09-10..13 in the previous
enclosure and had block 0 warning permanently for being *better* than its stale baseline. Daily medians from
the dashboard history, before (17–30 Sep) against after:

| block | AGC | noise/ms | jamInd | C/N0 best |
|---|---|---|---|---|
| 0 (L1) | 6318 → 5967 | 70 → 66 | **47 → 32** | 47–48 → 46–48 |
| 1 (L2) | 5616 → 5616 | 48 → 45 | **6 → 25** | — |

Both bands moved, in opposite directions, and block 1's AGC did not move at all. Satellites
*seen* is unchanged at 38 and C/N0 is unchanged, so the wanted signal is fine and this is an
interference change rather than a signal-path fault. The cost is small but real: satellites
*used* went 19 → 17 and the receiver's own `tAcc` went 1 → 2 ns.

Worth knowing before reading alarm into it: **block 0 was already in `warning` before the
move** (since 29 Sep, at jamInd ≈ 49) and block 1 flickered in and out. What is new is that
block 1 now sits there persistently. The state comes from the receiver's own jamming flag in
`UBX-MON-RF`, not from a threshold on `jamInd`, which is why block 0 can be flagged at 32 now
having been unflagged at 47 historically — it is the receiver's judgement, not a level.

**Leading hypothesis: harmonics of the 10 MHz distribution.** The 123rd harmonic of 10 MHz is
1230 MHz, 2.4 MHz from L2's 1227.6 centre and well inside the band; for L1 at 1575.42 the
nearest harmonics are 1570 and 1580, about 4.6 MHz out. The rubidium, the distribution
amplifier and all their coax now sit inside one metal box with the antenna feed, on new routing
and new bonding, which is a textbook way to change which harmonics couple into the front end.
Other candidates are the Si5351's 54 MHz and its 864 MHz VCO, the CM4's 125 MHz Ethernet
clocks, and the switching supplies now enclosed with everything else.

**There is no spectrum view to settle it with.** `UBX-MON-SPAN` would localise the carrier
immediately, but this firmware (TIM 2.01) does not implement it — the
`CFG-MSGOUT-UBX_MON_SPAN_UART1` key does not exist on the receiver, while `MON-RF`'s does. So
localisation needs either a spectrum analyser or substitution at the bench: change one thing at
a time — antenna coax routing away from the 10 MHz runs, termination of the distribution
amplifier's unused outputs, the single-point bond of the antenna shield — and watch `jamInd`
per block, which is itself the receiver's spectrum monitor.

## Dead RTC → gpsd → chrony

*2026-09-10.* The rig ran at stratum 3 from the network with both GNSS refclocks at
`Reach 0`. The antenna, receiver config, chrony config and permissions were all fine.

1. The IO Board's PCF85063 RTC had no backup cell (`rtc rtc0: Power loss detected,
   invalid time`).
2. Ubuntu's `fixrtc` set boot time from the filesystem date, about two years stale.
3. gpsd started at that bogus time.
4. Once NTP answered, chrony stepped the clock by 65,939,623 s.
5. gpsd survived the step but **never wrote another SHM sample**, though both processes
   stayed attached to the segment.
6. `lock NMEA` means PPS2 cannot number its pulses without NMEA, so both refclocks
   starved.

`chronyc tracking` showed a well-disciplined stratum-3 clock throughout. Only per-refclock
reach revealed the problem, which is why `gpsstat` exists.

**On Arch** `fixrtc` is gone, and systemd raises a clock that reads before its build
epoch, so a flat cell costs months at most. The step-and-starve failure still applies to
any large step. `gpsd-after-timesync` and `gpsd-shm-watchdog` restart gpsd automatically
([timing.md](timing.md#gpsd-self-healing)). A backup cell is tracked in plan.md.

## `lock NMEA` took PPS2 down with every gpsd hiccup

*2026-09-21.* `gpsstat` intermittently warned `refclock PPS2 reach 177 (filling or
lossy)` and `refclock NMEA reach 177` together, clearing on its own within minutes.

`refclocks.log` showed both refclocks losing samples over **identical** windows — same
timestamps, same durations, 36 to 113 s, between 10 and 35 times a day. They do not
share a path: PPS2 is the PHY's external timestamp on `/dev/ptp0`, NMEA is gpsd's SHM
segment. What joins them is `lock NMEA`, which is how PPS2 learns which second a pulse
belongs to, plus `maxlockage`, whose **default is 2 pulses**. Any gpsd SHM silence
beyond about 2 s therefore discarded every pulse in the window, although the pulses
themselves kept arriving at the PHY untouched. The watchdog journal confirmed the
trigger: `gpsd SHM(0) silent (check 1)` immediately before each gap. This is the same
coupling as the 2026-09-10 incident above, in miniature, and it only reaches the
watchdog's restart threshold when a silence lasts over two minutes.

Fixed by raising `maxlockage` to 60 in `config/chrony.conf`. A stale NMEA sample costs
almost nothing because it only fixes *which* second the pulse belongs to, and over 60 s
the rubidium-disciplined clock drifts about 1.7 ns.

Verified: a 37 s NMEA outage at 20:46:03 UTC produced **52 PPS2 samples and 16 NMEA
samples** in the same 50 s window. PPS2 no longer gaps with NMEA.

> **Keep the lock.** It is what makes a bad NMEA fail *closed*. Unlocked, PPS2 numbers
> pulses against the system clock, and both incidents on this page show what that costs:
> a clock two years stale in one, 527 ms off UTC at stratum 1 in the other. A 60 s fuse
> instead of a 2 s one, not no fuse.

## UART bandwidth is a correctness constraint

*2026-09-11.* Enabling RAWX + SFRBX at 38400 baud pushed the NMEA cycle from ~250 ms to
~919 ms after the second, past the ±0.5 s that second-numbering allows. chrony numbered
pulses to the wrong second and the clock sat **527 ms off UTC while reporting stratum 1
with a few-ns offset**. The servo was tracking the wrong pulse perfectly.

UART1 is now at 460800 (115200 until 2026-09-19). Before enabling any new output on
UART1, check the latency margin. `f9t-rawlog` measures it before and after and backs out
if the budget is exceeded.

## No reference at the Si5351

With CLKIN missing, the Si5351 does not go silent: CLK4 emits a stable ~11.6 MHz, the
CM4's internal PLLs never lock, and the board sits with power and green LEDs lit, doing
nothing. The firmware's CLK4 gating turns that into a clean "no clock". Check in this
order:

- **REF LED off:** no signal on CLKIN. Check the AR-40A output, pad, DA and cable.
- **REF on but CM4 dead:** read Teensy status on USB (or `/dev/ttyAMA5` if the CM4 is
  up). Look for `CLK4=on OE=0xEF`.

## CM4 won't boot

*2026-09-10.* A whole session went on a CM4 that "would not boot". The clock, firmware,
filesystems and the idea of F9T traffic on the console were all suspected. It was the
Ethernet cable. Shortcuts:

- If `rpiboot` sees the eMMC, XIN is good. Try it first.
- Any kernel message at all means ROM, bootloader and kernel ran.
- Incoming UART traffic cannot hang a Pi boot. A baud mismatch only makes a healthy boot
  *look* dead. There is no serial console, because GPIO14/15 belong to the F9T.

## Imaging the eMMC with rpiboot

- Jumper J2 1–2, micro-USB to J11, `rpiboot -d mass-storage-gadget64`, built from the
  usbboot repo. The gadget files are not in a packaged `rpiboot`.
- **usbguard** blocks the device when it re-enumerates. The symptom is `Device located
  successfully`, then `op_get_active_config_descriptor: device unconfigured` and a
  segfault. Stopping the daemon does not release an existing block, so allow the device.
- Disable desktop automount first. Auto-mounting ext4 replays the journal, which writes
  to the disk.
- A FAT dirty bit afterwards is a leftover, not damage. The `fsck.fat` boot-sector
  difference at offset 65 *is* that dirty flag.
- Remove the J2 jumper afterwards.

## ubxtool and UBX traps

- **Name the gpsd device** whenever more than one is attached, or readbacks silently
  come back empty. This once produced a survey that *reported* disabling TMODE while
  changing nothing.
- **`UBX-CFG-VALGET` returns at most 64 items.** Dumps must page with the `position`
  field. An unpaged dump truncated `CFG-MSGOUT` to 64 of 561 keys.
- ubxtool 3.25 ignores `-p MON-SPAN`. The spectrum view needs u-center.
- u-center access needs a maintenance window. gpsd owns ttyAMA0, and ser2net is disabled
  so it cannot compete for the port.
