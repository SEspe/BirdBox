# Session notes — 2026-09-29

Working notes for resuming. The durable record is `FSD_BirdBox_CHANGELOG.md`;
this file is the "where we stopped and what is still unproven" layer.

## Shipped today

| Version | FSD | What |
|---|---|---|
| 0.84.0 | v3.17 | Visit log records **why** an iNat frame failed (`err:<reason>`) |
| 0.85.0 | v3.18 | Classification **backlog is visible** (depth, peak, drops) |
| 0.86.0 | v3.19 | **Visit viewer** — a click opens the whole burst, not one frame |
| 0.86.1 | — | Fix: `getAttribute('href ')`, a stray space, made the click a no-op |
| 0.87.0 | v3.20 | UI page served `Cache-Control: no-cache, must-revalidate` |
| 0.88.0 | v3.21 | Fix: "last identified" named one bird while linking to another |
| 0.89.0 | v3.22 | Page reports when it is older than the firmware (banner) |
| 0.89.1 | — | Fix: visit thumbnails never loaded (hidden container + `loading=lazy`) |
| 0.89.2 | v3.23 | `/api/event` decodes its parameter; diagnosis post-mortem recorded |

Commits: `b771904` `72817e5` `eb4cb34` `68b99ef` `00bb28d` `3752dcf` `c12aaf7`
`0c7c66e` `b199bd9` `292af63` `1e25cfe`. master == origin/master.
**Nothing released on GitHub today** — 0.89.2 is committed and pushed only.

## Unit state

| | `.205` test | `.240` production |
|---|---|---|
| Firmware | **0.89.2** | **0.80.0** (deliberately untouched) |
| Camera | OV5640 | OV2640 @ HD |
| Home Assistant | enabled | not configured |
| Uptime at write | 227 s (**just panicked**, see below) | 293 055 s (3.4 days), 617 events |

`.240` was left alone all session at the user's instruction. Only `GET
/api/status` was ever called against it.

## The classification numbers (the day's real finding)

133 events, **68 named (51%)**, 65 unclassified. The 65 are **two unrelated
problems** that the log could not previously tell apart:

| Cause | n | share |
|---|---|---|
| Every frame errored — the box never got an answer | 34 | 52% |
| Mostly errored | 5 | 8% |
| Answered, every frame under the 25% threshold | 16 | 25% |
| Cleared 25% on one frame only (no 2-frame corroboration) | 9 | 14% |
| Cleared 25% on ≥2 frames but species disagreed | 1 | 2% |

**60% were transport failures, not classifier misses.** Ceiling if those were
answered: ~80% identified. Errors were a **burst**: 7 in hour 07, **32 in hour
08**, 0 in hour 09.

**Ruled out live, not by inference:**
- *Not* the rate limiter — `inatCooldown` was 0 across 55 samples; error runs
  outlast the 60 s cooldown (08:44:17→08:46:33, four dead events); real rate is
  ~15 req/min against a 100/min limit.
- *Not* memory or TLS — iNat buffers are all PSRAM; 12 real handshakes through
  `/api/inat-test` under load all succeeded, one at `heapIntBig8` 16 kB.

Leading unproven suspect: a **401 storm** (`inat_score()` refreshes the JWT at
entry; a flaky refresh fails every call until it takes — fits the duration and
the abrupt clean recovery). `err:401` in the log settles it.

**The 26 genuine misses are a framing problem, not a threshold problem.**
Inspected the images: birds sit at the frame **edges** while the centre is full
of seed; an edge-clipped bird reliably returns `Mammalia`. Median frame
confidence 5% against a 25% threshold. Only 3 of 22 held an accepted in-region
ID on a single frame (`Sitta europaea=65`, `Parus major=31`, `Pica pica=26`).
**Do not lower `INAT_SOLO_ACCEPT_PCT` (75)** — it sits deliberately above the
47–72% band where iNat flip-flops between look-alikes (settled v2.33/v2.34).

## Queue behaviour (answered on real traffic)

Peak **4 of 16**, ~148 s behind at the peak, **zero drops**. So the 16-deep
buffer is comfortable at present load and the 15 s enqueue wait was
deliberately left alone — counting first. Worth re-checking at a **dawn** peak,
which is busier than the midday window that was sampled.

`classify_submit_event()` waits 15 s for a slot then logs the event
unclassified and returns — i.e. the box silently discards work, against the
standing "never drop work" rule. `clsQDrops` now counts it.

## VERIFIED on hardware today

- `/api/event` returns a visit's true frame list, and **the same list whether
  asked via the visit's first frame or a middle one** (the live view passes
  `spFile`, usually mid-burst). Traversal input (`/etc/passwd`, `../`) rejected.
- Served-page inline JS parses with esprima after every UI change (86 kB, one
  block).
- `err:<reason>` success path unchanged: `Sitta europaea=87;98;…`,
  `Parus major=95;82;97`.
- Queue counters move on real events; `clsQMax` 16.

## NOT yet verified / still open

- **`err:<reason>` has never fired in the field.** Nothing has failed since
  0.84.0 was flashed, so the tag itself is unwitnessed. This is the gate on all
  framing work (see TODO "Blocked on evidence").
- **`.205` panicked spontaneously at 18:17:37**, ~3 h 45 min into an 0.89.2 boot
  with HA enabled — not an OTA, not a settings save. `guardReboots:0`, so not
  the heap guard. New data point on the open HA panic item.
- The HA-enable panic is **intermittent**: 1 panic / 1 clean on deliberate
  trials, so it is a *worse* reproducer than the OTA path (6/6), not a better
  one. Do not build a bisect on it.
- **15 "no bird" events today on weak evidence** — per-frame scores like
  `Mollusca?=3`, `Animalia?=36`, i.e. iNat guessing non-Aves at 2–12%. Real
  events filed as background. Part of the gated framing/threshold work.

## What cost the most time, and why

A "thumbnails do not appear" report took **four flashed releases** to diagnose.
It was a browser serving a cached **0.85.0** page the whole time.

1. Every curl test passed **raw slashes**; the browser sends `encodeURIComponent`
   output. The one test that did encode was mangled by **Git Bash** into
   `C%3a%2fProgram+Files%2fGit%2f…` (MSYS path conversion) — a false failure
   that was briefly believed and acted on. Pass `%2F` explicitly.
2. The staleness banner added in v3.22 **cannot fire on a page that predates
   it** — dead code in exactly the case it was built for. "No banner" proves
   nothing.
3. **The symptoms named the cached build all along**: a visible QUEUE badge
   (0.85.0) plus pre-0.86.0 click behaviour pins the version exactly.

**Standing rule, now in the FSD changelog and memory: for any web-UI report,
establish which build the browser is running before changing code.** The
reliable remedy is a cache-busting URL — `http://<ip>/?v=2` — which is a
guaranteed cache miss and needs no cooperation from the browser. Two of the four
releases fixed real defects the user was never executing.

Related: parsing the served page proves **syntax, not behaviour** — esprima
happily accepted `getAttribute('href ')`.

## Suggested next steps

1. Read a day's `err:<reason>` tags; confirm or kill the 401-storm theory. This
   unblocks the framing work.
2. Then, and only then, discuss framing: the misses are edge-clipped birds, and
   the detection zone is the user's call (never changed uninvited).
3. Re-check the queue peak at dawn.
4. Consider cutting a GitHub release — nothing has been released since v0.76.0
   while the fleet has moved a long way past it.
