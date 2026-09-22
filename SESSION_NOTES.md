# Session notes — 2026-09-22

Working notes for resuming. The durable record is `FSD_BirdBox_CHANGELOG.md`;
this file is the "where we stopped and what is still unproven" layer.

## Shipped today

| Version | FSD | What |
|---|---|---|
| 0.76.0 | v3.03 | Home Assistant integration over MQTT |
| 0.77.0 | v3.04 | Night sleep on the camera's own light reading |
| 0.77.1 | v3.05 | Fix: pausing detection blinded the sensor that ends the pause |

Released: **v0.76.0** on GitHub (asset `BirdBox_esp32s3_v0.76.0.bin`). 0.77.x is
committed and pushed but **not released** — worth cutting v0.77.1 once the dawn
wake is confirmed.

Commits: `e51fb0f` (HA), `c14d694` (backlog), `538b2a7` (night sleep),
`9820213` (night-sleep fix). master == origin/master, tree clean.

## Unit state

| | `.205` test | `.240` production |
|---|---|---|
| Firmware | **0.77.1** | **0.75.1** (two behind) |
| Camera | OV5640 @ UXGA | OV2640 @ HD |
| Home Assistant | **enabled, connected, publishing** | not configured (endpoint predates fw) |
| Night sleep | **mode 1 (pause + camera off)** | off |

`.240` was deliberately never touched. It can take 0.77.1 from its own OTA tab.

## VERIFIED tonight, on hardware, in real darkness

- Night sleep entered by itself: `dark` latched on the first ambient sample
  after the boot quarantine cleared (22:16:41), slept one 30 s tick later
  (22:16:57), `camAsleep:true`.
- `detect` stayed **true** through the transition — that is exactly the case
  that used to leave it stuck `false` and unrecoverable.
- MQTT: `connected:true`, 187 consecutive publishes at 60 s, zero errors.
- Whole served-page inline JS parses with esprima after every UI change.

## NOT yet verified — pick up here

1. **The wake probe.** First probe was due ~22:27 (10 min after sleeping). It
   should set `luma` to a real number in `/api/night`. Unconfirmed at shutdown.
2. **The dawn resume** (~07:00–07:30). The whole point of the feature.
3. **Deep sleep (mode 2).** Never exercised. `.205` is on mode 1.
4. **Home Assistant entities** actually rendering in HA — the publish path is
   proven, what HA does with the discovery configs is not.

Check in the morning:
```sh
curl -s http://192.168.10.205/api/night     # expect asleep:false, luma>threshold
curl -s http://192.168.10.205/api/status    # expect detect:true, events climbing
```

## Known issue, not fixed (2 lines, deliberately left for tomorrow)

**The boot quarantine also skips ambient sampling.** `motion_task()` checks the
pause branch first, then `continue`s on the quarantine branch *before* reaching
`decode_gray()`. So the box is ambient-blind for `detect_quarantine_s` after
every boot. Benign at the 60 s default — it self-clears, and that is what the
22:16:41 latch above shows — but the setting accepts up to **3600**, and an
hour-long quarantine would blind darkness detection for that whole hour. Same
root cause as v3.05, same fix: sample ambient in the quarantine branch too.
Left unflashed because it was found after the overnight test had started and
should not go out unverified.

## Camera "vertical lines" — answered, NOT a fault

Striped near-black night frames are **per-column amplifier offset** (fixed
pattern noise) amplified by maximum AGC in near-total darkness. Measured on the
same sensor, same day: night frame mean green **4.18/255**, daylight frame
**139.88** and visually pristine. Banding shows only where signal is near zero
and vanishes over lit areas. Three settings make it worse, all left untouched
because they are the operator's calls:

- illuminator `ir:0` on **both** boxes — scene is genuinely black, AGC maxes out
- `fshut:1` on `.205` — pins a short exposure and lets AGC compensate
- `ae_level:+2` on `.240` — raises the brightness target, so more gain again
- `denoise:0` on the OV5640, which has hardware denoise unused

A wrong hypothesis is worth recording: the first theory was that noise lifted
the frame mean above `AMBIENT_DARK_ON_THR` (35). Measurement said 4.18. The
threshold was never the problem; **measure before tuning.**
