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

---

# 2026-09-23 — overnight result

## What the night actually proved

The wake **probe** works: `luma` went from `-1` to real readings (12 → 16 → 62)
across the night, so `camera_wake()` → measure → `camera_sleep()` does cycle.
Sleep held 8 h 26 m with no reboots and `detect:true` throughout — the v3.05
split-brain fix survived a full night.

**Temperature, the operator's observation, confirmed and decomposed:**

| | camera | temp |
|---|---|---|
| `.205` awake, UXGA (yesterday) | on | 51–53 °C |
| `.205` asleep, UXGA | off | **38.2 °C** |
| `.240` awake, HD (control, never slept) | on | 37.2 °C |

`.240` drifted 41.2 → 37.2 overnight, so ~4 °C of the 14 °C drop is ambient
cooling and **~10 °C is the camera being off**. Asleep-at-UXGA ≈ awake-at-HD.

## The crash, and how the temperature trace found it

`resetReason:"panic"` at 07:17, `uptime` 109 s. The "dawn wake" in the log was
**not** the night module resuming — a panic clears `s_asleep`/`s_dark`, so a
crashed box is indistinguishable from a woken one by state alone. **Check
`uptime` and `resetReason` before believing a wake.**

Cause was mine, introduced in v3.05: `decode_gray()` writes shared
`s_rgb`/`s_cur` and mutates `s_px`, and v3.05 gave it a third caller on a
DIFFERENT task. While night-paused the motion loop (every 250 ms) and the night
task's probe decoded into the same buffers concurrently. Symptom chain, all in
the log: `luma:-1` → a `camera_sleep()` drain that could not complete, leaving
the sensor powered for a full 10-minute interval while the state read
"sleeping" (**+11 °C, 38 → 49**, invisible in every field except temperature) →
panic. Fixed in 0.77.2 / v3.06: exactly one decoder at a time, plus a
`camera_sleep()` retry and an asleep-side reconcile.

## Night noise — settled with a metric

Fixed-pattern noise is columnar; real scenes are not. Ratio of column-to-column
jitter over row-to-row jitter, same sensor:

| frame | mean | col/row ratio |
|---|---|---|
| day 17:08 | 139.9 | 0.41 |
| **night 21:48** | 4.2 | **7.14** |
| dawn 07:18 | 97.9 | 0.58 |

Striping appears only in the dark frame. **The camera is not faulty.**

## Still open

1. **A genuine dawn wake has never been observed** — the one chance was eaten by
   the panic. Next real test is tonight.
2. Deep sleep (mode 2) still never exercised.
3. Wake threshold is conservative: `luma` 62 at 07:07 was already well lit and
   the box stayed asleep (needs >90). It woke ~2 min after sunrise, so this is a
   tuning call, not a bug — and thresholds are the operator's.
4. Boot-quarantine ambient blind spot (TODO.md) still unfixed.

## Thermal characterisation (2026-09-23 afternoon)

Die temperature, both boxes, same weather, `.240` at HD as the control:

| condition | temp |
|---|---|
| HD, idle, no stream | **40–41 °C** |
| UXGA, idle | 51–53 °C |
| QXGA, idle | **57 °C** |
| UXGA + one live stream viewer | **69–72 °C** (peak 72.2) |
| camera powered down overnight | **38 °C** |

Two things worth keeping:

- **One attached Live-tab viewer is worth ~14 °C.** It shows as a *step*, not a
  ramp, which is how it was distinguished from sun: the control box did not
  move at the same moment. It is therefore the most effective runtime lever a
  thermal throttler could pull — and unlike resolution, it *can* be pulled at
  runtime.
- **A temperature drop after a settings change was nearly misattributed.**
  Resolution was raised UXGA → QXGA and the temperature fell 12 °C; the cause
  was the Live tab closing at the same time, not the resolution, which had
  moved the wrong way. Two variables changed at once. The control box is what
  separated them.

`.205` reverted to HD (`res` 6 → 3 + reboot, `resActive` confirms 3).
