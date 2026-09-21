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

*2026-09-15.* After a reboot the CM4 can come up with no Ethernet at all:

```
mdio_bus unimac-mdio--19: MDIO device at address 0 is missing.
could not attach to PHY
bcmgenet fd580000.ethernet eth0: failed to connect to PHY
```

The rest of the system boots normally; there is simply no network, and with no console login the box is
unreachable. Observed on several kernels, so it is not a kernel regression.

- **Trigger:** warm reboots. Cold boots have not failed.
- **Recovery:** switch the **whole 5 V supply** off for ~30 s (leave the rubidium's 15 V on), then on. A power cycle
  of the CM4 alone is not reliable, and neither is another reboot.
- **Why:** the BCM54210PE has no reset line that a warm reboot asserts, and the PHY driver writes `BMCR_PDOWN` when
  the interface goes down, so the next kernel has to wake a PHY that may no longer answer MDIO. The exact mechanism
  is still open; upstream tracks the same signature in
  [raspberrypi/linux#5497](https://github.com/raspberrypi/linux/issues/5497).
- **Ruled out here:** the GNSS PPS on the PHY's SYNC pin, back-powering of the CM4 from the other 5 V devices
  (3V3 measures 28 mV with the CM4's own feed removed), and the `brcm,powerdown-enable` device-tree property.

Plan reboots of the timing host for when someone can power-cycle the rig.

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
