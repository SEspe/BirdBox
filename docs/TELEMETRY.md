# Telemetry reference — what every debug field means

Every diagnostic field the box exposes, what it actually measures, what a
healthy value looks like, and what a bad one is telling you.

Written because several of these were misread during a long debugging session:
a field that looks obvious can measure something subtly different, and two of
them below are *specifically* the fields that caused wrong conclusions.

**Three rules that apply throughout:**

1. **Know whether a field is LIVE or a SNAPSHOT.** Some update on every pass;
   others are written only when something succeeds or fires, and then sit
   unchanged. A snapshot reading zero does not mean "nothing is happening".
2. **Values written on COMPLETION are stale during a stall.** `loopMs`,
   `grabMs`, `decMs` are all written when a frame finishes. While the box is
   stuck they show the last *good* frame, not the stall.
3. **A max-since-boot resets on reboot.** `guardReboots`, `resetReason` and
   `uptime` tell you whether a zero means "never happened" or "lost".

---

## `GET /api/motion` — the detector

The live view polls this; it is the fastest way to answer "is detection
working?".

| field | meaning | healthy | what a bad value means |
|---|---|---|---|
| `n` | triggers since boot | climbing in daylight | flat with birds present = detection is not firing |
| `a` | a capture is in progress right now | brief `true` spikes | stuck `true` = `capture_event` is not returning |
| `q` | boot-quarantine seconds left | `0` after the first minute | nothing triggers while > 0 — by design |
| `c` | 8×8 mask of the cells that fired **on the last trigger** | — | **SNAPSHOT.** Zeros mean "nothing has triggered yet", not "nothing moved" |
| `cap` | max cells a cluster may have (from the mount setting) | 40 close / 28 medium / 20 distant | — |
| `cells` | cells in the last trigger's winning cluster | 5–25 | **SNAPSHOT**, same caveat as `c` |
| `rej` / `rejN` | largest rejected cluster, and how many were rejected | `rejN` low | `rejN` climbing while `n` stays flat = the cap is too low for this mount; raise the mount distance, do **not** raise sensitivity |
| **`frames`** | **compared frames since boot — LIVE** | always climbing | **frozen = the detector is not running.** The single most useful field here |
| **`loopMs`** | gap between the last two compared frames | ~590 ms at HD | seconds = the loop is being starved. Written on completion, so an ongoing stall reads stale |
| `grabMs` / `grabMax` | time waiting for the camera | 0–2 ms | tens of ms = frames are not arriving |
| `decMs` / `decMax` | time decoding the frame at 1/8 | ~320 ms at HD | rises with resolution — this is why HD is the locked default |
| `decErr` | failed decodes since boot | `0` | non-zero = `decode_gray` is failing silently |
| `livePct` | % of the detection zone that changed this frame | 0–3 idle | — |
| `liveClust` | the dominant cluster's share of the zone | below `thr` when idle | — |
| `liveCells` | cells in that cluster | — | — |
| `thr` | % the cluster must beat to trigger | 2 at sensitivity 88 | `liveClust` consistently just under `thr` = sensitivity is marginally too low |
| `gstep` | this frame was suppressed as a global light step | occasional | constant `true` = the scene's exposure is hunting |
| `capMs` / `capMax` | time `capture_event()` blocked the detect task | a few seconds | **this runs inline in the detect task** — while it runs, no new events start |
| `subMs` / `subMax` | time the classifier handoff blocked the detect task | `0` | > 0 = the classify queue was full and the motion task waited (up to 15 s) |

**`livePct` / `liveClust` / `thr` together answer "why didn't it trigger?"** —
either nothing changed (`livePct` ~0), or something did but stayed under the
bar (`liveClust` < `thr`), or it was rejected as too big (`rejN` climbing).

---

## `GET /api/sysinfo` — the box

| field | meaning | healthy | notes |
|---|---|---|---|
| `heapInt` | free **internal** DRAM | > 90 kB | the scarce pool. TLS handshakes compete here — **not** the PSRAM-inclusive `heap` |
| `heapIntBig8` | largest free internal 8-bit block | ~32 kB | fragmentation shows here before free-bytes drops |
| `heapPsram` / `heapPsramBig` | free PSRAM, largest block | MBs | roomy; allocate large buffers here freely |
| `socTempC` | on-die temperature | 45–70 °C | > 75 shows red. Drops sharply when the camera sleeps |
| `resetReason` | why it last restarted | `power-on` / `software` | `panic` = it crashed. Check `uptime` to see if it is looping |
| `guardReboots` | heap-guard reboots | `0` | non-zero = the DRAM guard fired; `guardBlock`/`guardFree` hold that moment's numbers |
| `httpdSock` / `httpdSockMax` | HTTP sockets in use | well under max | at the cap, new requests are refused |
| `inatCooldown` | seconds of 429 rate-limit cooldown left | `0` | **> 0 is the only place a 429 shows.** Check this before blaming the network |
| **`lastInferenceMs`** | **the WHOLE event's classification time** | see below | **not** one iNat call — see the trap below |
| **`inatMs`** | **one iNaturalist round trip** | 1–3 s | this is the remote service's responsiveness |
| `clsModel` | active classifier | `iNaturalist online` | — |
| `clsQ` / `clsQMax` | classify queue depth / capacity | 0–4 of 16 | — |
| `clsQPeak` | deepest since boot | < 8 | a 60 s poll cannot see a burst; this can |
| `clsQDrops` | events discarded because the queue stayed full | **`0`** | non-zero = work was thrown away. Should never happen |
| `motionTriggers` | same as `/api/motion` `n` | — | — |

### The trap: `lastInferenceMs` is not iNaturalist's response time

`lastInferenceMs` is timed from the start of an event's classification job to
its end. It contains:

- reading **each** frame off the SD card
- **one iNat round trip per frame scored**
- the ROI crop decode, when one runs
- the cloud fallback, when it runs

So it rises *both* when iNaturalist slows down *and* when more frames were
sent, and by itself cannot say which. Measured on one day: **1478 ms** and
**57865 ms** for whole events.

Read it with two companions:

| | question it answers |
|---|---|
| `inatMs` | is iNaturalist slow **right now**? (outside our control) |
| `lastFrames` (`/api/status`) | how many frames did this event take to decide? |
| `lastInferenceMs` | what did the whole event cost? ≈ `inatMs` × frames + SD reads |

If `lastInferenceMs` climbs while `inatMs` is flat, the box is sending more
frames. If both climb together, the remote service is having a slow day.

---

## Home Assistant sensors (MQTT)

> Ready-to-paste dashboard cards for these are in [`HA-CARDS.md`](HA-CARDS.md).

One state message every 60 s; every entity reads a field out of it. These are
the ones worth graphing — a duty cycle or a slow drift is invisible in a
single reading.

| sensor | meaning | watch for |
|---|---|---|
| `detect_ms` | detect cadence actually achieved | climbing = detection is being starved |
| `detect_grab_ms` / `detect_decode_ms` | the two halves of a detect frame | splits "frames not arriving" from "processor taken" |
| `fast_gap_ms` | the fast burst's achieved inter-frame gap | drifting up = short visits start being missed |
| `inat_ms` | one iNaturalist round trip | the remote service's health |
| `classify_ms` | the whole last event's classification | rises with `inat_ms` **or** with frame count |
| `cls_queue` / `cls_queue_peak` | classify backlog, live and high-water | peak approaching 16 = events will start being dropped |
| `cls_drops` | events discarded | any increase is work lost |
| `visits` | cumulative visit count, with species/confidence as attributes | the trend; attributes say which bird |
| `contrast` | scene contrast — what decides day/night | ~50 day, < 25 dark. **Not** brightness: AGC defeats a mean-brightness test |
| `cam_fault` / `cam_recoveries` | camera health | `cam_recoveries` climbing is the early warning, long before a hard fault |
| `night` / `night_hold` / `cam_on` / `asleep_min` | sleep state and why it is awake | — |
| `cluster_cells` / `cluster_cap` / `rejected` / `rejected_max` | detection cluster sizing | `rejected` climbing with flat `visits` = cap too low for the mount |

`classify_ms` and `inat_ms` are **omitted** until something has been
classified, rather than published as `-1`. HA shows "unknown", which is the
truth.

---

## Quick answers

**"Is detection working?"** → `/api/motion` `frames` climbing. Not `n`, not
`cells` — those are snapshots and read zero for several different reasons.

**"Why didn't that bird trigger?"** → `livePct` (did anything change?),
`liveClust` vs `thr` (did it clear the bar?), `rejN` (was it rejected as too
big?), `q` (still in boot quarantine?), and `/api/night` `state` (asleep?).

**"Why is classification slow?"** → `inatMs` first. If that is normal, it is
frame count (`lastFrames`) or the queue (`clsQ`, `clsQPeak`).

**"Is the box healthy?"** → `resetReason`, `uptime`, `heapInt`,
`heapIntBig8`, `socTempC`, `cam_recoveries`, `clsQDrops`.

**"Did detection stop, or did the birds?"** → `frames` climbing plus
`/api/night` `dark`/`contrast`. A quiet dusk and a dead detector look
identical in the trigger count and nowhere else.
