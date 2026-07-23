# Session Notes — BirdBox (updated 2026-07-23)

## ✅ VERIFIED 2026-07-23 08:58 — cert-pin held overnight
Checked `/api/sysinfo` after a full overnight run on **0.74.31**:
- `guardReboots` **3** (unchanged from baseline) ⇒ heap guard never fired.
- `heapIntBig8` **31744** — byte-for-byte identical to baseline, zero drift
  (pre-fix it decayed toward ~28.7 KB within ~107 min).
- `uptime` **52475 s (~14.6 h)** unbroken, ~8× the old leak cycle, through
  118 motion events, active iNat classification (Kjøttmeis 97%), 0 SD remounts,
  0 cam recoveries, SoC 51 °C.

**Conclusion: the root-CA cert-pin (0.74.31) is the permanent fix for the
cert-bundle DRAM leak.** Nothing further to watch.

Original baseline + check procedure kept below for the record.

## Session 2026-07-23 — ⓘ popups, defaults, stats fixes, Debug rework, i18n Phases 1+2; **released v0.74.35 / .38 / .39**

Started 0.74.31, ended **0.74.39** — every step built, OTA-flashed to the
reference unit, verified live, committed + pushed to `master`. Three GitHub
releases cut: **v0.74.35**, **v0.74.38** (first tag through the bumped CI
actions, validated clean), and **v0.74.39** (current latest).

### What shipped
- **0.74.32 / v2.65 — per-setting ⓘ info popups.** Every Settings label got a
  circled-i opening one shared modal (`SINFO` key→[title, description, default,
  alternatives] table in the served JS; `sInfo()`/`ipClose()`, `#ipop` div).
  The long inline help paragraphs moved into the popups; only safety/cost
  warnings stay inline (iNat password plaintext, cloud costs + photos leave
  device). **Popup text verified against code, not copied**: sensitivity =
  motion.c ~11 %/6 %/1 % thresholds; conf threshold gates the CLOUD tier only
  (`cloud_util.c:134` is its sole consumer); SD cap prunes oldest day-folders;
  **Placement `mode` is consumed by NOTHING** — popup honestly says
  "informational only".
  **Also removed the dead "Periodic iNat re-scan" section** — `inat_periodic_*`
  lost its consumer in the 0.74.0 pivot; the toggle did nothing and its text
  referenced the deleted `inat-birds-v1.tflite`. NVS fields + server parses
  kept for old export files. `stLoad`/`stSave` refs to `stInat`/`stInatv`
  removed (a leftover `$g()` on a missing id kills every handler).
- **0.74.33 / v2.66 — compiled-in defaults retuned** (settings.c): cool-down
  3→**120 s**, frames/event 5→**4**, confidence 60→**30 %**, iNat location
  ""→**Oslo "59.91,10.75"**. SINFO Default rows updated to match. Stored NVS
  values win, so existing devices unaffected; applies on fresh provision/wipe.
- **0.74.35 / v2.67 — gallery 🔍 tooltip fixed**: said "else on-device model"
  (stale since the pivot); `/api/id-primary` is a pure iNat round-trip → now
  "identify bird with iNaturalist".
- **0.74.35 / v2.68 — Stats Today row-click shows that day's images only.**
  Bug was on both ends: `spImgs` never sent the scope AND `stats_list_images`
  always scanned every visit log. Fix: `stats_list_images` gained a `date`
  param (single-day log scan, mirrors `stats_collect_scoped`); JS sends
  `&scope=day` in the Today view; heading says "today". **Deliberately did NOT
  reuse `stats_scope_date()`**: its 48-byte query re-read truncates on an
  encoded `sp=` and would silently drop the scope. Verified live: day scope =
  2 Kjøttmeis images all from today; all-time = 10 across days.
  (0.74.34 existed transiently on-device as the tooltip-only build; both fixes
  committed together as 0.74.35.)
- **0.74.36 / v2.69 — Stats date picker.** The Today button became a dropdown:
  "Today" default (value '' — server resolves the date, keeping the ~1970
  guard), then every capture day from `/api/days`. All four stats endpoints
  take `&date=YYYY-MM-DD`, shape-validated (`stats_date_valid` — the string
  feeds a `/sd/log` filename); labels say "on <date>".
- **0.74.36 / v2.70 — species rows merge by Latin binomial.** Operator spotted
  two "Bokfink" lines: 16× `Bokfink` + 1× `Common Chaffinch` (cloud tier logs
  English; relabel vocab logs Norwegian; display localizes both identically).
  Latin is now the row identity when both sides have one; JSON `key` = latin;
  `stats_list_images` matches species OR latin. Verified: one row, n=17.
- **0.74.37 / v2.71 — Debug "Species ID" card rework** (operator audit: "31
  species Northern Europe — still in use?"). Findings: "31 of 31" was a
  tautology AND named the wrong list — the real Norway filter is
  `species_in_region()` = the **147-entry** NO_NAMES allowlist in
  species_i18n.c, not target_species.h's 31 (that's the relabel picker +
  cloud-prompt vocabulary, kept, honestly labelled); "Last inference" had NO
  writer since the pivot — repurposed as **"Last classification"**, the timed
  wall-clock of the event's whole tier cascade (set in `classify_task`);
  the ".tflite to /sd/model" fallback text removed. sysinfo: `clsRegion` now
  = allowlist size (147), new `clsRfilt`; `classify_region_matches()` deleted.
- **0.74.38 / v2.72 — i18n Phase 1** and **0.74.39 / v2.73 — i18n Phase 2**
  (see the i18n status section below for the full picture).

### New verification tooling (keep using this)
`esprima` (pip'd into the session scratchpad's IDF-python) now **parses the
served page's inline JS for real** after every UI flash — strictly better than
the old grep/bracket-count ritual. A naive bracket counter false-positives on
the gallery's `\'`-escaped onclick strings; a real parser doesn't. Also
cross-checked all 29 ⓘ icon keys ↔ SINFO table keys both directions.
For i18n: simulated the ENTIRE client pipeline in Python (fetch page +
`/i18n.txt`, HTMLParser text nodes, same `tX()` logic) → 191/195 static
strings translate, misses all deliberate — no browser needed.

### i18n status (Phase 1 shipped 0.74.38 / v2.72, operator-approved)
- Whole static HMI follows the Language setting; dictionary = `main/i18n.txt`
  (EN master, TAB-separated, `#cols` header, NO only so far), embedded +
  served at `/i18n.txt`, translated client-side, English fallback.
- **Every new/changed static UI string needs an i18n.txt row** or it shows
  English (rule recorded in FSD v2.72).
- **Debug pane content stays English permanently** (operator decision) —
  only its section headers translate. Excluded from Phase 3.
- **Phase 2 shipped 0.74.39 / v2.73**: the 29 ⓘ popup bodies translate via
  116 `@sinfo:<key>:<field>` rows in i18n.txt (t/d/def/alt, HTML allowed,
  per-field English fallback). Verified: all 29×4 resolve in NO.
- Pending, unscheduled: Phase 3 (dynamic status/confirm strings, minus
  Debug — that content stays English permanently).

### Open / notes
- **Fast-burst gap variance explained** (operator asked why it's now <1000 ms):
  it was never a constant — grab + JPEG + SD-save, dominated by OV2640
  auto-exposure, so it tracks scene brightness (~500–800 ms in daylight,
  →~1 s dim). The old "~940 ms" figure included the 200 ms `FAST_BURST_MS`
  delay removed in v2.61. Measured same-day drift: avg 524 (08:58) → 693
  (midday), zero firmware involvement. Clarifying note appended to the FSD
  v2.61 entry — don't re-investigate a moving `fastAvgMs`.
- CI actions bumped off Node 20 (checkout v7, upload v7, download v8,
  gh-release v3); push build green, **release job validated by the v0.74.38
  tag — fully proven, closed.**
- "Last classification" (Debug) reads "none since boot" until the first
  motion event after each reboot — expected, not a bug.
- SINFO Default rows must be kept in sync when a settings.c default changes
  (noted in a comment above the table).
- Unit still runs its own tuned NVS values (sens 65, conf 25, cool 100,
  4 frames, dzoom **1** — user turned zoom on at some point despite the
  zoom-hurts finding; their call, not touched).

## ⏭ Verify tomorrow (cert-pin held?)
Baseline snapshot **2026-07-22 20:37**: fw **0.74.31**, uptime **8027 s (134 min)**,
`heapIntBig8` **31744**, `guardReboots` **3**, `guardUptime` 6404 (the last *pre-fix*
guard fire), events 38, camFault false, healthy.

**The one definitive check:** `curl http://192.168.1.111/api/sysinfo` and read
**`guardReboots`** — it persists in NVS across reboots and was **3** at session close.
- Still **3** ⇒ the heap guard never fired ⇒ **cert-pin killed the ~107 min leak cycle.** ✅
- **> 3** ⇒ the guard fired again ⇒ the leak wasn't the whole story; check `guardBlock`/
  `guardUptime` for when/why, and reconsider (cloud tier still on the bundle?).
Also glance at `heapIntBig8` (should hold ~31744, not drift toward ~28.7 KB) and `uptime`
(should be many hours unbroken). No watch is running — the device self-records all of this.

---


Long session. Reference unit at **192.168.1.111**, now on firmware **0.74.30**,
`nordic` model long gone (classification is iNaturalist-online + optional cloud).
Started at 0.74.4; ended 0.74.30. Everything below is committed + pushed to
`master` (linear history) unless flagged otherwise.

## What shipped (all built, flashed, verified live)

- **0.74.5 / v2.42** — "No bird" from iNat's `iconic_taxon_name`: when no in-region
  bird is found and the top guess is non-Aves, file the event as **no bird** (state 3)
  instead of unclassified — but only if EVERY scored frame agreed.
- **0.74.7 / v2.43+v2.44** — **The box logs in to iNaturalist itself.** Form login
  (GET /login → scrape CSRF → POST /session → cookie → JWT), so neither the 24 h JWT
  nor the weeks-long cookie ever needs pasting. **OAuth is NOT an option** — every iNat
  OAuth flow needs a registered app (~2-month approval). **iNat login name is `steine`**
  (not SEspe, not the email). Settings gained username/password; password is plaintext
  in NVS, stated in the UI. See memory `birdbox-inat-self-login`.
- **0.74.8–11 / v2.45–v2.48** — **Four stacked bugs behind false "no bird" verdicts**,
  each masking the next: (45) require no Aves *anywhere*, not just rank-1; (46)
  `result_better()` compared a background score vs a bird score as equals, so 15%
  "no bird" beat an 11% cropped Dompap — bird evidence must always win; (47) recheck
  gave follow-up frames `roi_none()` so they were never cropped (the frame-ROI sidecar
  had the boxes all along); (48) recheck only rewrote a row on success, so it could
  never *clear* a wrong label — which masked 46/47 in testing.
- **0.74.12–14 / v2.49–v2.52** — **The 6-hour TLS death, root-caused.** ESP-IDF's
  cert-bundle CA callback (`esp_crt_copy_asn1` → `esp_crt_ca_cb_callback`) **leaks ~7
  internal-DRAM blocks per TLS handshake** (found via `HEAP_TRACE_LEAKS`). Tiny blocks,
  but they shatter contiguity until the largest block drops < ~29 KB and every HTTPS
  handshake fails → classification dies silently. Added a **heap guard** (reboot when
  the largest INTERNAL|8BIT block stays < 30000 at rest) + `/api/heapdbg` diagnostics.
  See memory `birdbox-tls-certbundle-leak`. **Use `allocBlocks`, not free bytes** — the
  leak is invisible in byte terms.
- **0.74.15–16 / v2.51–v2.52** — recheck won't clear a label when the calls never got
  through (`scored==0`); **guard threshold fixed** (was 40000, set from the boot value,
  which caused a reboot LOOP that killed capture via the 60 s quarantine — 30000 is just
  above the real ~28.7 KB failure point, + an 1800 s settle window so a guard reboot
  can't chain).
- **0.74.17–18 / v2.53–v2.54** — Live view: **Current** badge 1-min TTL + blanks on a new
  event; **Last ID** links to the event's best (peak-confidence) frame; yellow
  **CLASSIFYING** stage; the Last-ID link was inert (badge `pointer-events:none`).
- **0.74.19 / v2.55** — **Guard was firing on mid-call transients, not a real leak.**
  A live `score_image` transiently grabs a big block; a 10 s guard check landing mid-call
  saw a dip that recovered. Fix: guard **defers on `classify_busy()`** (measure at rest)
  + persists a snapshot to NVS so a reboot is self-explaining (`guardReboots`/`guardBlock`
  in /api/status). `sysinfo.heapIntBig` was a *different* heap query than the guard's —
  added `heapIntBig8` so monitoring matches.
- **0.74.20 / v2.56** — **Fast-burst backup frames.** 4 fast frames at the trigger, before
  the normal slow burst, scored ONLY if the slow frames fail (pooled with them on
  fallback). For quick, short-visit birds (a Granmeis that stops < 1 s).
- **0.74.21 / v2.57** — Live view shows the **post-event cool-down countdown**
  (reference unit's `cooldown_s` is **120 s** — a big invisible pause before this).
- **0.74.22 / v2.58** — Fast burst skips on false triggers (only fires if the slow frames
  actually saw a bird); FASTBIRD badge relabelled **"FASTBIRD CHECK"** + amber (it's a
  classification step, not a new detection — it appears mid-cool-down because classify
  is async).
- **0.74.23 / v2.59** — `lastFrames`/`lastFast` in /api/status + `tools/verify_fastburst.sh`
  (empirical CI check for the 4-fast + N-slow structure).
- **0.74.27 / v2.61** — Fast burst grabs **back-to-back (no delay)** + Debug shows the real
  measured "Fast-burst gap".
- **0.74.31 / v2.64** — **Cert-pinning — the PERMANENT leak fix.** `inat.c` uses a pinned
  3-root mini-bundle (USERTrust RSA + GTS Root R4 + GlobalSign, embedded from
  `main/inat_roots.pem`) via `.cert_pem` instead of `esp_crt_bundle_attach`, bypassing the
  leaky CA callback. **Verified: allocBlocks flat across 8 handshakes (was +7/call), TLS +
  classification intact.** The ~107 min guard cycle should be gone; the guard is now a pure
  safety net. iNat's two hosts use different CAs (api=Sectigo/USERTrust, www=Google/GlobalSign)
  so all three roots are needed. Cloud tier still on the bundle (infrequent). Memory
  `birdbox-tls-certbundle-leak` updated.
- **0.74.30 / v2.63** — **Gemini ✨ fixed.** `gemini-flash-lite-latest` now 400s on
  `thinkingConfig.thinkingBudget=0` ("Request contains an invalid argument", generic, no
  field detail — bisected). Removed `thinkingConfig`; verified (Dompap 95% in 4.3 s). Not
  quota (that's 429). Widened error capture so full 4xx messages reach the Gallery tile.

## Reverted / dead ends (don't re-attempt without new info)

- **Keep-alive TLS reuse (v2.60, reverted v2.62).** Aimed at the cert-bundle leak by
  reusing one connection across an event's frames. Committed with benefit unconfirmed;
  live use was the verdict: **it doubled classification time (~9.6 s → ~16.8 s/call on a
  healthy heap)** because iNat/Cloudflare idle-closes the socket between calls, so every
  reuse paid dead-socket latency for no saved handshake. Operator noticed classification
  "takes forever, longer than cool-down." Reverted to per-call init/cleanup.
- **Fast frames at lower resolution (VGA).** Tested and reverted: VGA frames (confirmed
  7× smaller, 19 KB vs 158 KB) capture at the **same ~1000 ms** — the **OV2640 frame rate
  at 20 MHz XCLK (~1 fps) is a hardware floor, resolution-independent.** The nominal 200 ms
  fast gap is **unreachable** on this sensor; best ever seen was ~535 ms (cold heap),
  ~1190 ms typical. Removing the delay didn't help either. Proven three ways.

## Key facts / gotchas established this session

- **iNat call latency ~9.6–10 s/frame** is network + iNat-server time, not the box (heap
  healthy, timing rock-steady). At 5–9 frames that's 50–90 s/event — inherent to
  iNat-online. Operator **reduced `capture_count` 5 → 3** late in the session to keep
  events inside the 120 s cool-down (4 fast + 3 slow = 7 max; still ≥2-frame corroboration).
- **The cert-bundle leak is still present.** The heap guard mitigates it (reboot ~every
  107 min under load; instrumented, self-explaining). The durable fix is now **cert-pinning**
  (pin iNat's root to bypass the leaky CA-bundle callback) — NOT connection reuse. Weigh the
  cross-signed-root fragility (memory `esp-tls-cross-signed-root`).
- **Edge-clipped birds are iNat's blind spot.** A bird cut off at the frame edge reads as
  `Mammalia`/background — no crop or resolution trick recovers it (the fix is framing).
  Also: iNat confuses the two *Perisoreus* jays and only ever offers the American
  *canadensis* (geo-rejected) for a Lavskrike; and it won't reliably split Granmeis/Løvmeis.
- **`version.h` one-liner trap:** a Python `open(p,"w").write(open(p).read()...)` truncates
  the file (write opens/truncates before the read arg evaluates). Bit me 3×. Use the Write
  tool for version.h, not an inline read-modify-write.
- Build must run in **PowerShell** (idf_tools export), not the Bash tool (MSYS rejected).

## Open / next steps

1. **Watch that the cert-pinning killed the reboot cycle** — with 0.74.31, `guardReboots`
   should stop climbing (it was 3 at session end) and uptime should run for many hours.
   If it still cycles, the leak wasn't the whole story.
2. **Watch `capture_count=3`** — does classification now stay inside the 120 s cool-down,
   and are more single-good-frame birds lost? (operator's live experiment)
3. ~~`gemini.c` 3 init vs 2 cleanup~~ — **investigated, NOT a leak**: the 3rd "init" was a
   grep matching a comment; both functions pair init→cleanup on every path. No fix needed.
4. **frameroi sidecar not pruned** (append-only; small; FSD v2.03).
5. Device: 0.74.30, cloud provider **OFF** (`cprov=0`) — the ✨ button works regardless;
   Gemini key stored + now verified working again.
