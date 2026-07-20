# Session Notes — BirdBox (updated 2026-07-20)

The architecture changed fundamentally since the last notes: **the on-device
TFLite-Micro model is gone** (0.74.0 / FSD v2.37 "nordic teardown"). Species ID
is now **online only** — `inat.c` (iNaturalist CV) as the primary tier with a
Norway geo-filter and multi-frame corroboration, escalating to the paid cloud
tier (`cloud.c` → Claude or Gemini) when iNat declines.

Reference unit: **192.168.1.111**, firmware **0.74.5** (it briefly lived on
192.168.10.236 — always confirm with `GET /api/status`).

(Everything before 0.73 — the three-tier cascade, nordic-v0.8, the socket-pool
sluggishness fix — is in git history + memory and not repeated here.)

## Where the classifier landed

- **iNat is the auto-classifier.** Region allowlist (`species_in_region`)
  rejects geographically-impossible top hits (v2.28); per-frame scores land in
  the visit log's `pf` column (v2.29); ROI crop is pixel-tight with a best-of
  whole-vs-crop pick (v2.30/v2.31); a single very-confident frame can
  solo-accept at **≥75%** (v2.33/v2.34).
- **The JWT no longer needs a daily paste** — `inat_refresh_jwt` renews the 24 h
  token from a stored `_inaturalist_session` cookie (v2.35/v2.36).
- **The relabel-picker vocabulary is `target_species.h`**, not model labels.
- `training-data/` is now historical: the retrain pipeline and its artifacts are
  kept, but nothing on the device consumes them. `docs/MODEL.md` carries a
  DEPRECATED banner.

## Why the on-device model was abandoned (don't re-litigate)

Usable accuracy needed per-species retraining, and the only obtainable training
images are glossy, subject-filling "hero" shots that don't transfer to a fixed,
low-res feeder view. Measured: iNat-weight backbone init **~80%** vs ImageNet
**~92%** — features tuned to hero shots actively hurt. See memory
`inat-backbone-init-hurts`.

## Crop resolution: a closed dead end (tested twice)

- **v2.39 (0.74.2)**: decoding the crop at full res gave **no gain** — iNat's
  `score_image` resizes every upload to ~299² internally, so the existing ~360²
  half-res crop already saturates its input. Granmeis got *worse* (19→15%), and
  a full-res q90 crop **HTTP 500s** at iNat (forcing a quality drop that hurt
  the hardest case). Reverted.
- **v2.41 (0.74.4)**: re-tested on the specific theory that it would help a
  *small* corner bird. A Kjøttmeis clipped at the frame edge went 4%→7% but
  stayed **Unidentified and the wrong species**. Reverted again.
- **Lesson:** for a clipped or badly-posed bird, no crop/resolution trick
  helps. The fix is framing, or the cloud tier. Do not attempt a third time.

## This session — 0.74.5 / FSD v2.42, "no bird" from the iconic taxon

Many "unclassified" events are **background** (swaying branch, shadow), not a
missed bird — and iNat already says so: every candidate carries
`iconic_taxon_name`, and on an empty scene the **top** guess is a plant or
mammal. We only used that field to *reject* non-birds; now:

- `inat.c` — when no in-region bird is found and the **top** result is non-Aves,
  emit **"no bird"** (`cu_to_result(bird=false)`) instead of "Unidentified bird".
- `classify.cpp` (`inat_event`) — file the event as **"no bird"** (gallery state
  3) only when **every** scored frame's top guess was non-Aves
  (`ncand==0 && unid==0`). Conservative by design: a bird seen in *any* frame
  keeps the event **unclassified** (state 0) for human review.
- A "no bird" verdict is **final** — not escalated to the paid cloud tier (same
  role the old nordic background guard played).
- `top_label[]` is repopulated with the raw iconic names after `cu_to_result`,
  so the `pf` column shows what drove it (`Plantae?=6;Mammalia?=4`).
- Recheck (§3.4) inherits the behaviour, so re-running a day also buckets its
  background. No route or settings change.

**Status: built, OTA'd, device confirmed on 0.74.5.**

## Open / next steps

- **v2.42 is not yet empirically confirmed** — it only affects newly classified
  or rechecked events. Operator chose to **wait for live events** rather than
  recheck today. What to look for in the Gallery: vegetation/shadow triggers
  landing as **"no bird" (state 3)** with a `pf` like `Plantae?=…`, and the
  unclassified pile shrinking to actual unidentified birds. If real birds start
  getting filed as "no bird", the all-frames-agree guard is too loose.
- Today's log (2026-07-20) still holds a large `st:0` pile from before the
  change; a recheck would re-bucket it if that's ever wanted.
- **frameroi sidecar not pruned** — `/log/frameroi-YYYY-MM-DD.csv` grows
  append-only, not cleaned when old captures age out (small; FSD v2.03).
- **Per-day visit-log split** was the fix for gallery/relabel SD-bound latency —
  shipped in v2.07; see memory `birdbox-per-day-log-files` for context.
- Cloud tier: Gemini key stored and verified, model `gemini-flash-lite-latest`
  (`gemini-2.5-flash` and the whole 2.0 generation 404 on this key — see
  memory `birdbox-cloud-classifier-dual`).
- `training-data/` artifacts + logs remain uncommitted by convention.
