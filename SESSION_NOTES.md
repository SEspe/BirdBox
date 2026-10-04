# Session notes — 2026-10-04

Working notes for resuming. The durable record is `FSD_BirdBox_CHANGELOG.md`;
this file is the "where we stopped and what is still unproven" layer.
Earlier sessions are in git history (`54d7bdf` for 2026-10-01).

## Shipped today

| Version | FSD | What | GitHub release |
|---|---|---|---|
| 1.1.1 | v3.42 | (from 10-01) SD self-test — pushed + released today | v1.1.1 |
| **1.2.0** | **v3.43** | **Daily GitHub update check + header note** | v1.2.0 |
| **1.3.0** | **v3.44** | **Stats aggregate cache (6.7 s → 0.2 s)** | v1.3.0 |

`master` == `origin/master`; **v1.4.2 is the Latest release** (see below).
Releases were missing since v0.99.2. v1.1.1-v1.4.1 were created by hand with
`gh release create`, and each asset checked byte-identical after download —
**but CI's tag-triggered job then replaced every one** with its own build and a
template body. Firmware unaffected (same commit, same IDF); notes regenerated
from the changelog. Fixed for good in 1.4.2: CI owns releases.

## Later the same evening: 1.4.0 → 1.4.2

| Version | FSD | What |
|---|---|---|
| 1.4.0 | v3.45 | `/api/days` counts cached (was 9.3 s per Gallery/Maintenance/Stats open); Stats asks `names=1` |
| 1.4.1 | v3.46 | consistency sweep of both caches; reset-point race fixed |
| **1.4.2** | **v3.47** | **host unit tests (159 checks, ASan/UBSan) + CI owns releases** — no functional change in normal operation (operator accepted the two edge-case differences) |

**Releasing is now: version commit on master → `git tag vX.Y.Z && git push origin vX.Y.Z`.**
CI tests, builds, checks the UI JS, verifies tag == version.h, takes notes from the changelog
and publishes. Never `gh release create` (CI overwrote v1.1.1-v1.4.1 that way; notes restored).

## TOMORROW (2026-10-05): see the unit in operation

`.205` was still on 1.3.0 at the end of the session; the operator updates via the header note.
1. `/api/status` → `version` 1.4.2, `resetReason`/uptime sane (`/api/sysinfo`).
2. Open `http://192.168.10.205/?v=142` (cache-bust).
3. Time it: `/api/days` (expect well under 1 s after the first minute of uptime),
   `/api/days?names=1`, and a Stats all-time view (3 requests, expect ~0.2 s).
4. Day counts correct: compare `/api/days` `n` with `/api/events?date=` counts for a
   past day and for today; check today's count rises as frames are saved.
5. Stats invalidation: relabel/confirm one image → Stats counts move at once.
6. Post-boot Stats anomaly (6.2 s / 2.5 s right after boot on 1.3.0): does it recur?
7. Update check: `latest` 1.4.2, `updAvail` false, `updErr` "".

## Units

| | fw | state |
|---|---|---|
| `.205` | **1.3.0** | healthy; updated by the operator FROM the new header note. NOT yet on 1.4.x at session end - the operator updates tomorrow |
| `.240` | 1.1.0 | **offline.** Something else answers ping at .240 (DHCP moved it?). Predates the update check — needs one manual OTA to ≥1.2.0. |

## 1.2.0 — the update check

- `main/update_check.c`. An `esp_timer` asks every 5 min "is a check due?"
  (24 h since last success, 1 h after a failure, WiFi up, real clock, not
  classifying, no OTA running) and only then spawns a short-lived 8 kB task
  that calls `api.github.com/repos/SEspe/BirdBox/releases/latest` and exits.
  No permanent internal-DRAM cost. Timestamps in `RTC_DATA_ATTR` so night mode
  2's deep-sleep wakes don't re-check.
- Cert **bundle**, not pinned: one handshake/day makes the ~7-block leak
  noise; pinning would die silently at GitHub's next CA change. Measured
  internal free 114775 → 114275 B across one check, largest block unchanged.
- Generic User-Agent, **no version** sent (§9 no-telemetry posture).
- `/api/status`: `latest`, `updAvail`, `updAgeS`, `updErr`. Header note
  `#updN` → `goOta()`; OTA tab status line `#updSts`; i18n rows added.
- Release only counts with a plain `X.Y.Z` tag AND a `.bin` asset.
- **Verified:** a 1.1.0-labelled build saw 1.1.1 as newer 305 s after boot;
  the served `updShow`/`goOta` were exercised under `cscript` (no node on this
  PC) against stub elements; and finally **the operator's own test** — reboot
  on 1.2.0, note appeared for 1.3.0, updated from the OTA tab. First check
  sometimes lands on the 2nd tick (~10 min) when the first finds the box busy.

## 1.3.0 — Stats speed

- Cause (measured): the tab fires daily/species/hourly together; each re-read
  every `visits-*.csv` (~2.2 s on 15 days), httpd serialised them → done at
  2.26 / 4.50 / 6.75 s. HA re-ran the same scan every 15 min.
- Fix: `storage_visit_log_gen()` (bumped AFTER every rewrite: relabel, batch,
  confirm, day reset, stats reset, migration, remount, `rc_rewrite_row`) and
  `storage_visit_log_appends()`. `stats.c` caches past days (`hist`), the
  finished result (`full`) and the single-day view (`day`) under one mutex.
  Plus a 16 kB PSRAM stdio buffer, files sorted by name, and **the daily series
  now keeps the NEWEST 62 days** — it used to freeze on the first 62 (`.240`
  had 53 days and would have hit it in ~10).
- **Measured, settled:** all-time view **~0.2 s** (214/202/209 ms), single
  requests 0.07-0.10 s, day view 0.24 s first / 0.06-0.11 s warm.

## Still open (also in TODO.md)

- **Post-boot anomaly:** seconds after boot, all-time took 6.2 s, then 2.5 s.
  2.5 s means a hist rebuild where a hit was expected. Unexplained. Next step:
  expose last-rebuild ms + count in `/api/sysinfo` rather than reading serial.
- Cache invalidation not yet exercised on hardware (relabel → Stats must move;
  midnight rollover; Reset Statistics).
- From 10-01, still open: SD banner can't fire with NO card; sdhealth.csv
  unreadable over HTTP; `capMs` full-day re-measure; sdtest busy-refusal untested.

## What I got wrong today

1. **Shell quoting cost two attempts.** A big heredoc with mixed quotes broke
   bash; files with mixed CRLF/LF line endings broke exact-match Python edits.
   Writing fragments to the scratchpad with the Write tool and using the Edit
   tool on mixed-EOL files worked first time.
2. **Inserted lines by number after earlier inserts had shifted them** — two
   counter bumps landed in the wrong place (one inside an `#if`). Caught by
   printing every site's context before building. Insert bottom-up in ONE pass,
   or anchor on text, never on stale line numbers.
3. **Released 1.3.0 before it ran on hardware** — at the operator's request (to
   test the update note), and the changelog said so until measurements replaced
   it. The settled numbers held; the post-boot ones did not fit the model and
   are recorded as unexplained rather than explained away.

4. **Created releases by hand while CI also creates them on the tag.** Both
   ran; CI won, silently replacing assets and notes on five releases, and my
   "verified byte-identical" report was true for only a few minutes. Found only
   because the operator asked about CI. Lesson: before automating around a
   pipeline, read what the pipeline already does.
5. **A Python one-liner with mismatched quotes** wrote nothing, but the build
   step after it ran and passed - so the version was bumped while the
   changelog was not. Caught by checking the FSD header afterwards. Check each
   step's exit status, not just the last one's.
6. **Unit tests passed first time** - which proves nothing on its own. A
   deliberate regression (reverting the 62-day fix) on a throwaway branch made
   CI fail on exactly the three expected checks. Do that once for any new suite.
