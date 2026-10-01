# Session notes — 2026-10-01

Working notes for resuming. The durable record is `FSD_BirdBox_CHANGELOG.md`;
this file is the "where we stopped and what is still unproven" layer.
Earlier sessions are in git history (`c379b43` for 2026-09-30).

## Shipped today

| Version | FSD | What |
|---|---|---|
| 1.0.2 | v3.40 | Removed the two **redundant** directory walks (read + write) |
| **1.1.0** | **v3.41** | **Hour buckets on the card; logical paths unchanged** |

`.205` on **1.1.0**, `.240` untouched on 0.80.0.

## The finding: the SD card was never degrading

Yesterday's dashboard read "Frame read from card" rising 852 → 1409 ms over one
day and concluded the card was worn. **It is not.** `s_sd_ms` starts before
`fopen`, so it times path resolution, and FATFS resolves a name by scanning the
directory linearly. Measured by fetching capture files at known positions in a
2647-file day folder:

| position in dir | TTFB |
|---|---|
| 2 | 23, 28 ms |
| 1300 | 227, 232, 223 ms |
| 2640 | 429, 423, 438 ms |

**0.155 ms per preceding entry, dead linear.** Classification did `fopen` *and*
`stat(path)` — two walks: 2 × 0.155 × 2647 = 821 ms predicted vs 876 ms
measured, leaving ~55 ms for the actual 110 kB read (~2 MB/s, a healthy card).
The 852 and 1409 figures were the same card at 09:00 and at 16:00.

## Measured, same folder size (~2650 files)

| | |
|---|---|
| single-frame save, 1.0.1 | 1.728 / 1.805 / 2.031 s (mean **1.85**) |
| after v3.40 (one less walk) | 1.310 / 1.347 / 1.341 / 1.345 (mean **1.34**) |
| after v3.41 (buckets) | 1.008 / 1.031 (**~1.01**) |
| spread | 0.30 s → **0.04 s** — the variance *was* the walk |
| old-day image read, with the memo | **0.026 s** |

At ~6.8 frames/event that is ~5 s off `capMs`, all inside the detect task.
First post-change event: `capMs` **7994** (was 9641 / peak 18467).

## The layout, in one line

Physical `/captures/<day>/<HH>/<name>.jpg`; **logical** `/captures/<day>/<name>.jpg`
is what the visit log, every URL, every API parameter and the retrain export
still carry. `storage.c` alone knows the difference, derives `HH` from the
filename, and falls back to the flat location — so nothing migrated and all 15
stored days still work.

**A lookup memo was necessary.** The first build tried the bucket then fell back,
which made every pre-v3.41 capture pay a failed bucket open (a full day-folder
scan) first: browsing an old day went 0.23 → 0.63 s. A 24-bit per-day mask of
which hour folders exist now picks the order. It is **advisory** — read/written
without a lock — so both helpers still try the other layout if the first misses;
a stale mask costs a scan, never a 404.

## Verified after flashing

- `/api/days` byte-identical across all 15 days (374 … 2991 files).
- `/api/events` lists all 2654 pre-existing files plus new ones, in order.
- Legacy flat frames and bucketed frames both serve and both group through
  `/api/event` (confirmed Blåmeis burst; Kjøttmeis burst).
- **Full event end to end:** captured into bucket `17`, logged with the logical
  path, classified Kjøttmeis **95%**, 1 upload / 1 call (early exit), all 5
  frames grouped, image serves.
- Two OTAs with `haen=1`, `resetReason` `software` both times — the v3.36 fix
  continues to hold (now 5/5).

## Still open

- **The read-side win is predicted, not yet measured.** Today is a transitional
  folder: 2654 flat files *and* the new hour directories, so resolving the
  directory `17` is itself a 2654-entry scan and a bucketed read still measures
  ~0.43 s. **The test is a bucketed read tomorrow** on a folder holding only
  ~24 bucket directories — it should fall to tens of milliseconds. If it does
  not, the model is wrong and the whole v3.41 rationale needs re-checking.
- `capMax` was the dominant unexplained cost and is now partly explained
  (directory walks were ~5 s of it). Re-measure over a full day.
- `append_visit_line()` still does a `stat()` on `/log` before appending —
  same bug class, ~57 ms per event. Located, not fixed (see TODO).
- Run-time stats still not enabled; per-core CPU still unmeasurable.
- `.240` is on 0.80.0 and **lost its SD card** (`sdPresent:false`,
  `sdRemounts:0`, last capture 2026-09-30 12:06). Untouched — operator's call.

## What I got wrong today

1. **Truncated `main/version.h`** with `sed -n '...' -i` — `-n` plus `-i` writes
   an empty file. Restored from git immediately; no loss. Use plain
   `sed -i 's/a/b/'`, never `-n` with `-i`.
2. **Shipped the bucket fallback without thinking about the legacy cost.** It
   doubled old-day browsing (0.23 → 0.63 s) and only showed up because the
   post-flash check happened to time a legacy file. The memo fixed it, but the
   regression was predictable from the design and was not predicted.
3. Guessed at a 0.43 s bucketed read before testing the direct physical path;
   the direct test settled it in one request. Same lesson as yesterday's.
