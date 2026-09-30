# Session notes — 2026-09-30

Working notes for resuming. The durable record is `FSD_BirdBox_CHANGELOG.md`;
this file is the "where we stopped and what is still unproven" layer.
The 2026-09-29 session is in git history (commit `511e90e`).

## Shipped today

| Version | FSD | What |
|---|---|---|
| 0.91.0 | v3.25 | Detect frame split into its two waits (grab vs decode) |
| 0.91.1 | v3.27 | Time the work the detect task does inline (capture, handoff) |
| 0.92.0 | v3.28 | Pin detect task to core 0 — **did NOT help**, recorded as a negative |
| 0.93.0 | v3.29 | **Stop uploading frames once the verdict is decided** (operator's idea) |
| 0.94.0 | v3.30 | iNaturalist's own latency (`inatMs`), + `docs/TELEMETRY.md` |
| **0.95.0** | **v3.31** | **Pin TCP/IP to core 1 — the one change that worked, +31%** |
| 0.96.0 | v3.32 | Debug rows get (i) buttons; **`tools/check-ui-js.py`** pre-flash gate |
| 0.96.1 | — | All 45 Debug rows documented, not just the new ones |
| 0.97.0 | v3.33 | Pacing 1100→400 ms; card read timed |
| 0.97.2 | v3.34 | Report frames **uploaded** not saved; (i) no longer toggles checkboxes |
| 0.98.0 | v3.35 | `detect_zoom` default 0→1; stale "zoom hurts" advice retired |
| **0.99.0** | **v3.36** | **HA panic family fixed** — never publish from the MQTT task |
| 0.99.2 | v3.37 | OTA with `haen=1` confirmed clean, 2/2 |

PR #1 merged (squashed, linear history kept). `.205` on **0.98.0**, `.240`
untouched on 0.80.0.

## The one real win: pin the network stack off the detection core

```
before (no pin, HA off):  3676 ms/frame   16% of capable
after  (TCP on core 1):   2551 ms/frame   23% of capable    +31%
```

`classify_task` (priority 3) could never have starved `motion_task`
(priority 4) — but **lwIP, at priority ~18, carries its TLS uploads**, and
`CONFIG_LWIP_TCPIP_TASK_AFFINITY_NO_AFFINITY` let it follow the work onto
core 0. Pinning *motion* alone (v3.28) did nothing for exactly that reason.
Now `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU1=y`. Needs the generated `sdkconfig`
deleted.

## Measured, and worth keeping

| | |
|---|---|
| card read, one ~110 kB frame | **852–854 ms**, once **1814 ms** (~130 kB/s — slow) |
| iNat per call | 4.4–6.3 s today (1–3 s is normal) |
| an 8-frame event | 60.3 s total, of which **8.8 s was our own pacing gap** |
| frames uploaded per event | **1 of 8 saved** after the early exit — working better than expected |
| detect cadence, idle | ~590 ms/frame at HD |
| `capMax` | 17–27 s — **still the dominant unexplained cost** |

## Hypotheses KILLED (do not retry)

- PSRAM bandwidth starving the camera DMA
- decoder contention with `classify_crop_jpeg`
- the classify queue wait (`subMs` is flat **0**)
- motion on the wrong core (pinning it alone changed nothing)
- **MQTT** — plain `mqtt://` on 1883 to a LAN broker, one payload a minute;
  stalls persist unchanged with `haen=0` (worst `loopMs` 9712 with HA off)

## Still open

- **`capMax` 17–27 s.** `capture_event()` runs inline in the detect task.
  Note it calls `detect_once()` itself to know when the visitor leaves, so
  "detection stops during capture" is **less true than it was stated** — it is
  detecting, just not free to start a new event.
- ~~HA must stay OFF on `.205`~~ — **FIXED in 0.99.0, HA is back on.** The
  cause was `MQTT_EVENT_CONNECTED` calling `publish_discovery()` +
  `publish_state()` **inside the esp-mqtt callback**, i.e. on esp-mqtt's own
  6 kB task, with ~2.9 kB of buffers on it. Announcing now happens on `ha_task`
  (stack 6144→8192). **That one bug explains the whole family**: the OTA
  panic (6/6), the settings-save panic, the spontaneous one 3h45m into a boot,
  and the 0.94.0 boot loop — all moments of extra memory pressure against a
  stack that was already marginal. Verified **2/2 clean OTAs with `haen=1`**,
  `resetReason` `software` both times. The `haen=0` workaround is retired.
- SD card is slow and inconsistent (852 → 1814 ms). Worth trying another card.
- Run-time stats are **not** enabled (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`),
  so per-core/per-task CPU still cannot be measured. That is the instrument
  that would have answered the whole duty-cycle question in one request.

## What I got wrong today (five things)

1. **"CPU-intensive classifier"** — wrong framing. Priority 3 cannot starve
   priority 4; it was the *network stack* at 18.
2. **"Detection stops during classification"** — overstated. `loopMs` reports
   the last *completed* gap, so stalls show up in samples taken after they end.
   Stalls also occurred with `clsBusy` false.
3. **`/api/capture` timing as a per-frame cost proxy** — it is not; SD write and
   HTTP dominate (HD ~2.57 s vs QSXGA ~2.6–3.2 s despite a 5× smaller JPEG).
4. **My own arithmetic from the Debug screenshot** — treated `lastFrames` as
   frames *scored* (it is frames *saved*) and `inatMs` as an *average* (it is the
   *last* call). Overshot the next measured event by 84% — then I shipped the
   same confusion into the UI, which is what the operator caught.
5. **Flagged `dzoom` as harmful** from memory without reading the changelog.
   v2.31 repurposed it: whole frame is always scored first, the crop is only a
   fallback, so it **can never score below whole-only**. Default is now 1.

**The pattern: the changelog is authoritative, memory is a hint.** Check the
former before acting on the latter.

## Also shipped late in the day

- **The HA reporting interval is a setting** (`ha_interval_s`, 30–600 s,
  default 120, was a fixed 60) after the operator noticed it was not exposed.
  There is no single right value and the (i) says so: nothing published moves
  meaningfully inside a minute, and the peak/cumulative fields carry their own
  extremes — so 30–60 s while diagnosing, 300–600 s for monitoring.
- **`detect_zoom` defaults ON** (v3.35). The old default and the Settings advice
  both described pre-v2.31 behaviour where the crop *replaced* the whole frame.
  It has been a fallback since v2.31 and can never score below whole-only.

## Process changes that stuck

- **`tools/check-ui-js.py`** parses the inline script out of `build/BirdBox.bin`
  *before* flashing. Added after a duplicated `drowi()` tail reached the device:
  the esprima check had caught it, but only because it ran *after* the flash.
  Now step 2 of the verification list in CLAUDE.md.
- **`docs/TELEMETRY.md`** documents every field, and is explicit about which are
  live and which are snapshots written only on success — the distinction behind
  four separate wrong conclusions this week.
- Every Debug row now carries an (i) with meaning, healthy range, and what a bad
  value indicates.

## Suggested next steps

1. **`capMax` 17–27 s** — now the last big unexplained cost.
3. Consider enabling run-time stats temporarily to settle per-core load.
4. Re-measure detect cadence when iNaturalist is fast again; some of today's
   pain was theirs (4.4–6.3 s/call against a 1–3 s norm).
5. `.240` is still on 0.80.0 and can take a release when convenient.
