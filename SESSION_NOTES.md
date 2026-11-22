# Session notes — 2026-11-22 (on top of 2026-10-29)

Long day. `master` is at **1.5.26** (`v1.5.26`, released). `v1.5.25` and `v1.5.17` through
`v1.5.24` were released along the way. Nothing here is unpushed except what is listed under
"Still open".

## Releases and merges

| Version | What | Where |
|---|---|---|
| 1.5.17 | Panic core dump kept in flash (`coredump` partition, serial flash needed); route `GET /api/coredump`. Merged from `coredump-1.4.5`, renumbered from 1.4.5 / v3.50 | `master` |
| 1.5.18 | Field tuning as firmware defaults: sensitivity 88, cool-down 30 s, HA interval 30 s | `master` |
| 1.5.19 | Night mode on by default (pause mode, `sleep_mode = 1`) | `master` |
| 1.5.20 | OV5640 auto-gain ceiling capped at 8x (`0x3A19 = 0x80`), after every camera init | `master` |
| 1.5.21 | Ceiling lowered to 4x (`0x40`). An edge-skip change in the night stats was tried and reverted (it hid part of the image) | `master` |
| 1.5.22 | HA device name setting `ha_name` (default `BirdBox`) | merged in 1.5.26 |
| 1.5.23 | Reset Statistics moved to Maintenance; Refresh statistics button on Statistics | `master` via `stats-refresh` |
| 1.5.24 | Settings / Advanced settings split (in-page dropdown) | `master` via `stats-refresh` |
| 1.5.25 | Settings menu in the top bar, like Stats (Settings / Advanced settings) | `master` (`v1.5.25`) |
| 1.5.26 | Merge of `ha-name`, with test-only focus routes removed (they had been swept into that branch) | `master` (`v1.5.26`) |

Renumbering: `ha-name` was 1.5.22 / v3.74 and became 1.5.26 / v3.78 in the merge. v3.74 was
already taken on master, and `camera.c` comments were corrected to v3.73.

Branches now:
- `master`: 1.5.26.
- `stats-refresh`: merged into master, pushed.
- `ha-name`: merged into master (its test routes were removed in the merge).
- `coredump-1.4.5`: merged as 1.5.17. Still exists locally and on origin; can be deleted.
- `focus-assist`: pushed (scripts and TODO only). Its focus-lock code is in
  `git stash` (`focus-lock test code`), not on any branch and not released.

## Test units (MAC → address, as of end of day)

- `192.168.10.35` (MAC 28:84:85:65:…): 1.5.26, account `SteinE`. Shared iNat account with .36 until .36 changed.
- `192.168.10.36` (MAC 28:84:85:65:68:f4): 1.5.26, account `Maanite64` (password entered on unit). Sensitivity 81, cool-down 30 s. Night mode on. Detail zoom off.
- Board MAC 44:1b:f6:ca:ab:10: 1.5.26 at `192.168.10.38`, static (saved). This is the board the setup tests ran on.
- Board MAC 28:84:85:65:5c:d8: the board that went silent and wouldn't flash reliably; it sometimes showed as `192.168.10.37`/`.223`. Unplugged for the day.

## Classification and iNaturalist (test units)

- `.36` on its own account: 25 identified / 75 visits in the morning; cool-down blocks dropped after the switch.
- `.35` on the shared account: far more cool-down blocks (185 against 76) in the 10:00 and 12:00 hours. Calls per visit about the same (4.8 against 4.7). Cause: shared account rate limit; `.35` hits the cool-down more. Fixed by giving it its own account (still to do on .35).
- Many visits are low confidence on birds that are clearly there (for example a great tit at 3–13 %). Not an iNat fault; threshold/geometry question.

## Motion false positive

- A sun/shadow edge on the feeder floor triggered motion (no bird in the frame). Cause: sensitivity 88 gives a 2 % per-cell threshold (`1 + (100 − sens)/10`). At 70 it's 4 %; at 81 the same 2 % as 88.
- Set `.35` and `.36` to 70, then 81 (as asked). Watch the next day for shadow-edge triggers. If they return, go back to 70.

## Focus assist (investigation, not for release)

- Idea: a sharpness meter, like old 35 mm focus aids (split-image / microprism are optical and need a prism; this sensor has none). Digital equivalent: gradient energy of the frame, peaking at best focus.
- Method works in principle: the score rose to a clear peak and fell on both sides (first run, cup as object).
- Problems found: lens positions not marked; camera moved sideways with lens turns; light changed during runs (score moved with brightness); exposure drifted (fixed by a register-level lock, `0x3503 = 3` confirmed); the stream was slow after boot and sometimes returned old buffered frames.
- Score disagreed with the eye on one frame (scored 35 on a frame that looked sharp). Centre crop is mostly low-contrast wall.
- Next time: mark the lens at quarter-turns, clamp camera and body, lamp on the object for the whole run, lock exposure from the start, score a region around the object normalised for brightness and contrast.
- Details in TODO.md under "focus assist" on `focus-assist`.

## Settings UI

- Settings menu in the top bar (Settings ▾ → Settings / Advanced settings), like Stats. Advanced holds cloud keys (Claude / Gemini), camera tuning, image/stream options, storage, NTP, illuminator, iNat session/token fields. Advanced shows a warning. Hiding changes only the page; saved values unchanged.
- Reset Statistics is under Maintenance; Statistics page has Refresh statistics (a browser refresh opens Live).

## Hardware problems (board-specific)

- Two S3 boards (one with MAC 28:84:85:65:5c:d8, the other with 44:1b:f6:ca:ab:10) showed an onboard PCB antenna that is sensitive to touch. Ping with a finger near the antenna: 15 % loss, 85 ms average. Without: 0 % loss, 2 ms.
- Finally: wiggling the board changes the link, so it's a physical fault (loose or cracked antenna feed / joint), not firmware.
- The silent / won't-flash behaviour came from USB and power instability once WiFi starts (USB port drops, COM number changes, download mode after resets). Flashing worked when the chip answered on a stable port.
- Camera: the new boards have no camera module fitted (`camera probe failed: not supported`), so the firmware runs without one. Expected.
- The flash slot confusion: a 1.4.4 write reported verified but slot 0 held 1.5.26 when read back. Read back the header (`v1.4.4` / build date) to confirm which image is in a slot before trusting a flash.

## Decisions made with the operator

- Settings split into Settings / Advanced (keys and rarely used tuning under Advanced).
- Night mode on by default (pause mode, not deep sleep).
- Device name in HA is a setting; test units named `BirdBox` and `BirdBoxWroom` on `.36`.
- Iuser for `.36` is `Maanite64`; `.35` still on `SteinE`.
- Focus-assist routes stay out of releases.

## Still open

1. Give `.35` its own iNaturalist account (its cool-down count is the shared-limit problem).
2. Antenna / connector on the S3 board(s) with the intermittent link: reflow or replace. Use a board with an external antenna connector for field units.
3. Setup-network test on a working board: the phone joined `BirdBox-Config` but the setup page never showed the address on the board tested. Retest on a good board.
4. Settings menu: verify clicks in a browser (served page checked; behaviour not clicked).
5. Night mode check on `.35` and `.36` next dark evening (pause, not sleep, and no false wake from the edge light).
6. Focus test: redo with the notes above, when there's time.
7. `coredump-1.4.5` branch can be deleted (merged as 1.5.17).
8. `focus-lock` stash: keep or drop.
# Session notes — 2026-10-29 (on top of 2026-10-12)

Branch `best-visits-stats`, pushed to origin. `master` untouched (still 1.4.4),
no tag. Test unit `.205` runs this branch's build (1.5.10), not coredump.

## Shipped: Year statistics card (1.5.5 → 1.5.10)

The Year statistics view (best visits per species, per calendar year) was already
committed at the start of the session as 1.5.3–1.5.5. This session turned its
species popup into a card that matches the Live view's reference card.

| Version | FSD | What | Where it is in git |
|---|---|---|---|
| 1.5.6 | v3.57 | Row click no longer redraws the Live card: lookup split out as `spLookup(la, done)`; `refShow` = lookup + card render | in `320aaf3` (not committed alone) |
| 1.5.7 | v3.58 | Species panel shown as a card at the top of the view; the panel node moved between two hosts (`spiHost`) | in `320aaf3` |
| 1.5.8 | v3.59 | Year card replaced by the Live reference card itself, same size; `refEls(k)` flag picks Live or Year | in `320aaf3` |
| 1.5.9 | v3.60 | Card to the left of the two photos (`.bvrow` flex row, card kept outside `#bvList`) | `320aaf3` |
| 1.5.10 | v3.61 | Below 560 px the card stacks above the photos | `a3431b3`, pushed |

**Commit caveat:** `320aaf3`'s message names only 1.5.9, but its diff holds
1.5.6–1.5.9, because I left those changes uncommitted until asked. The
changelog (v3.57–v3.60) is the per-version record. Read `git log` for 1.5.10
and newer; do not expect a commit per version before that.

**Decisions made with the operator:**
- "Popup" meant the species panel (photo scroller, details), not the Live card.
  Asked once with options; operator chose "split the lookup" first.
- The Year card's minimize state is its own key (`bbYearMin`); the Live card's
  stays `bbRefMin`.
- The Year card's name opens the Live species overlay (`spiOpen(g_yrCur)`), as
  the Live card does.
- Fetch failure now clears the card instead of keeping the previous bird (v3.57).

**Verification (what is proven and what is not):**

| Check | Result |
|---|---|
| build + `check-ui-js` gate | clean for every version |
| OTA to `.205` (1.5.6, 1.5.7, 1.5.8, 1.5.9, 1.5.10) | `OK` each time; `software` reset, `guardReboots` 0 |
| served page grep | new ids/functions present each time |
| operator's browser | sent a screenshot of 1.5.9: card beside the two photos, layout renders |
| narrow-window rule (<560 px) | **not seen in a browser yet** |
| card minimize/restore, name → overlay | **not clicked in a browser by us** |

No JS test runner on this PC, so the browser-side logic is checked only by the
gate (syntax) and by the operator's eyes.

## Where things stand

- `.205`: 1.5.10, the branch build. No coredump partition, so the serial flash
  for coredump is still pending (see 10-28 state).
- `.240`: not looked for this session.
- Branches: `master` 1.4.4 (`v1.4.4` tag); `best-visits-stats` `a3431b3`
  (pushed); `coredump-1.4.5` `8b76726` (unmerged). Merge order: operator decides.

## Open

1. Browser check of the narrow layout at `http://192.168.10.205/?v=160`
   (narrow the window below 560 px), plus minimize/restore on the Year card.
2. Name click on the Year card → Live species overlay: not yet clicked.
3. Coredump serial flash on `.205` (needs USB, daylight, COM port check).
4. Host unit tests (`test/host/test_best_core.c`) still have never run locally.

## Lessons

1. **Commit per version when asked to commit, or say which versions a commit
   holds.** Bundling 1.5.6–1.5.9 under one message made the git history
   misleading. The changelog carries the detail, but the commit should name it.
2. **`python` on this machine is the Microsoft Store alias.** The UI gate must
   use the IDF Python: `C:\Users\Stein\.espressif\python_env\idf6.0_py3.11_env\Scripts\python.exe tools\check-ui-js.py build\BirdBox.bin`.
   The CLAUDE.md line `python tools/check-ui-js.py` does not work here.
3. **The Bash tool blocks `sleep` chains.** Use a status call after the OTA
   instead (it came back in one go after the 120 s upload).
4. **Memory went stale within a day.** The 10-28 state file still named
   `bdb215b` as the branch head; the real head was `866968f` then. Check
   `git log` before trusting a branch note.

---

# Session notes — 2026-10-12 (on top of 2026-10-06 and 2026-10-04)

## 2026-10-12: 1.4.3 — HA visit labels (released)

**Trigger:** the operator saw `.205`'s one visit of the day as *Skjære* in Home
Assistant but *unclassified* on the Stats tab.

**Stats was right.** The visit (08:35:17 first frame, 08:35:22 log row - same
visit; the Gallery names the first frame, Stats the row written at the end)
was a magpie on the back rim with its **head above the frame in all 8 shots**.
pf: `err:401` (daily JWT expiry; refresh worked) then Pica pica 16/22/15/15/12 %,
Toppmeis 6 %. A frame votes only at >= `conf` 25, so five agreeing frames still
gave zero votes. Cloud fallback is off (`cprov=0`). Read-only re-score via
`/api/id-frame-debug` today: 15 % and 5 % - not a transient.

**The HA bug (fixed, 1.4.3 / FSD v3.48, commit `7fd7057`):** the visits
sensor's `species`/`confidence` attributes were the last IDENTIFICATION, which
an unclassified visit leaves alone - so it showed the previous day's 16:11
Skjære. Also the count moved only on the 15-min log re-read while attributes
moved every publish. Now `classify_last_visit()` -> `visit`/`visit_conf` in the
state message, visits attributes read those, and a new visit triggers the log
re-read at once. `species`/`sp_conf` renamed "Last identified species /
confidence" (same uniq_id). FSD §13's stale per-species-sensor paragraph
rewritten.

| Check | Result |
|---|---|
| build + UI JS gate | clean |
| OTA `.205` 1.4.2 -> 1.4.3 | `software` reset, HA connected, `visits` 5525, heapIntBig8 32 kB |
| CI on tag `v1.4.3` | host tests, build, release all green; Latest = v1.4.3 with `.bin` |
| **HA hover after a real visit** | **NOT YET** - operator checks; broker password unknown to us |

heapInt read 94 kB ~1 min after boot (1.4.2 on 10-06: 116 kB at steady
state, not the same moment) - not a comparison; re-read at steady state.

**Classification of headless/edge birds - operator's call, nothing applied:**
an agreement rule (>=3 frames same top-1 below threshold, with a floor,
in-region only), the cloud fallback for unclassified only, or tilting the
camera up. See the unclassified-two-causes memory: 75 % solo and the
pica/hudsonia band are settled; don't reopen those.

**Also today (not BirdBox):** Discussions enabled on `SEspe/smarthomesec_ha`.
BirdBox shows 0 watchers - operator told how to Watch + email delivery. `gh`
lacks the `notifications` scope.

**Stale file:** untracked `resumetext.txt` (09-28) still says "disable HA
before flashing" - retired since 0.99.0. Delete or rewrite.

---

# Session notes — 2026-10-06 (on top of 2026-10-04)

## 2026-10-06: 1.4.2 verified on `.205`

The operator updated `.205` from 1.3.0 to 1.4.2 through the header note, then
asked for a controlled reboot. Results (full detail in changelog v3.45/v3.44):

| Check | Result |
|---|---|
| boot health | `software` reset, `guardReboots` 0, heapInt 116 kB, big block 32 kB |
| update check | `latest` 1.4.2, `updAvail` false, `updErr` "", ran 311 s after boot |
| `/api/days` | **0.08 s** (1.3.0 that morning: **11.7 s**); `names=1` 0.08 s |
| counts vs walked files | equal on 08-02, 09-26, 10-03, 10-05, and today x3 |
| count tracking | `POST /api/capture` +1, deleting that snapshot -1, walk agreed |
| warm-up | first `/api/days` of a clean boot, at 80 s uptime: 0.08 s |
| Stats tab open | 0.12-0.15 s; first open after boot 1.94 s (the one cold scan) |
| post-boot anomaly | **not reproduced** |

Before the update, 1.3.0 had run 47 h through two midnights: all-time vs
per-day totals 20/20 days equal, totals 4069 = 4069 = 4069.

**Still open:** Stats moving after a relabel/confirm (operator's labels - waits
for them), and Reset Statistics. `.240` still not found (`birdbox.local`
answers as `.205`; nothing at .240).

**Measurement gotcha seen today:** the first readings after the update were
skewed by the operator's browser running the CACHED 1.3.0 page, which still
asked `/api/days` WITH counts - an ~11 s walk that my requests queued behind.
Same family as the stale-UI-after-OTA rule: know which page is running.

---

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
