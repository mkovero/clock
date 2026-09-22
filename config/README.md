# config

Receiver and daemon configuration for the rig.

## Not here: anything carrying the position

This repository is public. The antenna's surveyed position is decimetre-accurate and
identifies the building, so the files holding it are kept in the private `mkovero/sys`
repository under `clock-private/`, and are listed in `.gitignore` here:

| File | Why it carries the position |
|---|---|
| `f9t-ppp-position.txt` | the PPP solution as `CFG-TMODE` keys |
| `f9t-known-good.txt` | used by `f9t-restore` at boot; carries the TMODE keys too |
| `survey-*.meta` | standalone survey results |
| `ppp-*.sum` | NRCan CSRS-PPP reports — ECEF, lat/lon and UTM throughout |
| `f9t-config-{ram,flash}.txt` | full CFG dumps, which include TMODE |

The copies the rig actually reads are in `~/f9tcfg/` on aika. `f9t-restore` applies
`~/f9tcfg/f9t-known-good.txt` at every boot, so **a position change must be written to
both that file and `f9t-ppp-position.txt`**, or the next boot reverts it.

`tools/f9t-skymap` reads `~/f9tcfg/f9t-ppp-position.txt` when given no coordinates, so it
needs no copy here.

> The public git history still contains the position in commits before 2026-09-22.
> Purging it would require a force-push rewriting every SHA, which was judged not worth
> breaking existing clones for.

## Here

| File | What |
|---|---|
| `chrony.conf` | copy of `/etc/chrony.conf` on aika |
| `f9t-timing-changes.txt` | receiver changes applied since the 2026-09-10 baseline, with reasons |
| `ar40a-trim-log.txt` | every move of the AR-40A mechanical trimmer |
| `f9t-reminders` | date-gated reminders surfaced by `gpsstat` |
