# Session Notes — BirdBox (updated 2026-07-16)

Rolling notes for the "BirdBox" work. Firmware is now **0.70.4**, FSD **v1.98**.
Everything committed to `master` (linear history); **0.70.4 is live** on the
reference unit at **192.168.1.111**, flashed via OTA and verified. Nothing this
session is tagged/released yet — **0.69.0 through 0.70.4 are on master only**,
no CI release.

This session was **five bug reports in a row**, all found via `bugreport.txt`
and live testing on the device. Theme: the "web UI feels slow / SD seems slow"
complaint turned out to be **the single httpd task being blocked by slow inline
handlers**, not the SD card (which idles at ~309 KB/s, same as flash-served pages).

## Current device state (verified live)
- **version 0.70.4**, clock `ntp`, SD present, `camFault:false`.
- **Resolution: HD 1280×720** — `res=3` (request) **and** `resActive=3` (actual)
  now agree (that split is the 0.70.2 fix). Earlier it had silently degraded to
  XGA (`res=2`); see §2.
- **Free internal DRAM ~180 KB**, **free PSRAM ~720 KB** — steady; the async
  identify worker and the events send-buffer are transient and leak-free.
- **`conf` (classify threshold) = 60** — still very high (real birds under 60%
  log as *unclassified*). Likely an old NVS-wipe reset from the tuned ~17.
  User's call — carried over, not touched.
- **Cloud ID still installed but idle** — no API key (`ckey_set:false`, `cld:0`).
- **Device address: 192.168.1.111** (confirm via `GET /api/status`; CLAUDE.md's
  `192.168.10.236` is stale).

## What we did this session (0.70.1 → 0.70.4)

### 1. Gallery: labels on a busy day's newest images reverted to "unclassified" — 0.70.1, FSD v1.95 (70cafd8)
- **Reported:** multiselect 16 of the latest frames → Set species → Dompap;
  progress bar completes, then the tiles reappear unclassified on refresh.
  Example `2026-07-15_21-52-28-722.jpg`.
- **The write was never the problem** — the label reached the visit log and
  `/api/labels/confirmed` returned it (retrain export had it). Only the gallery's
  *read* dropped it.
- **Root cause:** `gal_build_labels` loads the day's label table into a fixed
  `GAL_MAX_LABELS` array and stops at the cap. The day had **2527 captures** and
  `/api/events` returned exactly **1199** labelled tiles — pinned at the 1200 cap.
  The month CSV is read **front-to-back**, so the dropped rows are always the
  day's **newest** — exactly the range a user labels. Hidden until a day got big.
- **Fix:** cap 1200→**6000** (bound is *captures* per day, not events — relabel/
  copy-last append a row per no-row frame). Paid for by **interning** the
  localized species name in a per-day pool (`gal_intern`) and deriving `confirmed`
  from `state` — per-label cost ~104→35 bytes, ~210 KB PSRAM vs a ~550 KB block.
  Hitting the cap now logs a warning (the whole failure mode was silent).
- **Verified:** day returned 1256 labelled tiles (no longer pinned); the 16 images
  came back as confirmed Dompap.

### 2. Camera resolution silently flipped HD→XGA and lied about it — 0.70.2, FSD v1.96 (6d4e4fe)
- **Reported:** resolution changed off HD on its own, shown as **"Current (2)"**
  in the Settings dropdown (2 = XGA 1024×768 — a value the HD/SXGA dropdown can't
  produce).
- **Root cause:** `camera_init`'s **degrade ladder** (steps down the RES table
  when a size won't init — right, it prevents a boot-loop → OTA rollback) then did
  `g_settings.resolution = idx`, **overwriting the user's request with the
  fallback**. `/api/settings` serves that field and `h_settings_post` ends in an
  unconditional `settings_save()`, so **any** later save from **any** tab
  persisted the degrade to NVS. One bad boot + one unrelated save = HD preference
  destroyed for good.
- **Why it degraded:** `camFault` was true when an `/ota/upload` reboot (a **soft**
  reset) inherited the wedged OV2640 (only a power cycle clears it). Next cold boot
  took HD first try with ~720 KB PSRAM free — **capacity was never the issue**,
  Claude classifier not implicated.
- **Second bug (why it stayed invisible):** `camera_framesize_str()` returned the
  *setting*, so Debug/`camRes` read "HD" while the sensor ran XGA.
- **Fix:** split request from reality. `g_settings.resolution` is only ever the
  request (every boot retries it; no save can launder a fallback into NVS); what
  actually initialized lives in `camera.c`'s `s_active_idx`, exposed via
  `camera_active_res()` and reported by `camRes`. `/api/settings` gains
  `resActive`/`resActiveStr`; the Settings tab shows an amber warning when they
  diverge, distinguishing "not rebooted yet" from "failed at boot."
- **Verified:** forced the divergence (saved SXGA without reboot → `res:4,
  resActive:3`, `camRes` truthfully "HD"); restored HD.

### 3. Species-ID endpoints froze the whole web UI — 0.70.3, FSD v1.97 (be092c5)
- **This is the real answer to "the SD seems slow."** It isn't the SD.
- **Root cause:** `esp_http_server` serves every socket from **one task**, and four
  handlers ran their slow work **inline** on it: `h_classify_run`/`h_classify_file`
  block for a full on-device inference (**11–20 s** with the 5.5 MB TFLM arena),
  `h_claude_file`/`h_claude_test` for a TLS handshake + Anthropic round-trip
  (~1–5 s). While any ran, httpd couldn't `select()` other sockets → the entire UI
  hung. Proven by forcing one `/api/classify-file` and watching an unrelated
  `GET /` stall for exactly the reported `durationMs` (5.1 s), then recover instantly.
- **Fix:** offload all four to a worker task, mirroring the existing `/stream`
  async pattern (`httpd_req_async_handler_begin` → `ident_task` → return). Worker
  runs at **priority 4, below httpd's 5**, so a CPU-bound inference can't starve
  request handling. **Single-flight** (`s_ident_busy`) — TFLM already serializes
  on the run mutex and the gallery fires identifies one at a time; a collision
  returns `503 {"error":...}` (the tile prints it). JSON replies byte-identical,
  no UI change.
- **Verified:** `GET /` held at **30–55 ms while a 7.8 s inference ran** (was 5.1 s);
  single-flight returns one 200 + one 503; 5 sequential runs leak-free.

### 4. Batch gallery relabel + `/api/events` trim — 0.70.4, FSD v1.98 (d4a1f6f)
- **Batch relabel — the big win.** Bulk "Set species" fired one `/api/relabel`
  per image, and each relabel rewrites the **entire ~600 KB monthly CSV**, so a
  43-image op was 43 full rewrites (~4 min, UI-blocking — the sluggishness seen
  live during a bulk classify). New **`POST /api/relabel-batch`** +
  `storage_relabel_batch()` apply one species to many images in a **single** CSV
  pass (replace matching rows, append rows for images with none), O(CSV + N) not
  O(CSV × N). Gallery posts in **300-file chunks**. **Verified:** 5 images in one
  6.5 s rewrite vs ~32 s before; `applied:5, requested:5`, still confirmed in the
  export, served-page JS intact. Route count 45→46 (cap 56).
- **`/api/events` trim — partial.** Dropped the per-file `stat()` (its only use
  was the "· NN KB" tile label — user confirmed uninteresting; `s` field removed)
  and buffered the response (flush at 4 KB instead of one `send_chunk` per file).
  **12 s → 8.3 s** on a 2380-file day. **The rest is SD-bandwidth-bound** and is a
  known deferred issue — see below.

## Key decisions
- **Request vs reality must never be conflated** (0.70.2) — the degrade ladder
  keeps you from a boot-loop but must not rewrite the saved preference or report
  the fallback as the setting. Same discipline as never letting an inferred value
  read as fact.
- **Slow work never runs on the httpd task** (0.70.3) — the `/stream` async
  pattern is the template. Worker below httpd priority + single-flight.
- **Batch over fan-out** (0.70.4) — one CSV rewrite for N images (chunked at 300),
  not N rewrites and not a socket fan-out.
- **`bugreport.txt`** is the running tracker (untracked): bug 1 fixed 0.63.0,
  **bug 2 (resolution flip) marked fixed 0.70.2**.
- **File size in the gallery is not interesting** — user confirmed; removing the
  per-file stat() is a pure win.

## Gotchas (still live)
- **Whole UI is one inline `<script>`** — one JS syntax error kills all handlers;
  the C build can't catch it. Grep the served page + do a bracket/ternary balance
  pass. (Checked this session for `gRelabelBatch`.)
- **The httpd server is single-task.** Any handler that blocks (inference, TLS,
  a big SD read/rewrite) freezes the whole UI until it returns. `/api/events`
  (~2.2 s + readdir) and `/api/confirm`-per-image are still inline — the next
  candidates if UI-stall complaints continue.
- **OV2640 latches after a soft reboot** — a `camFault` sensor stays wedged
  through `/ota/upload`/`/api/reboot`; only a **hard power cycle** clears it. An OTA
  can therefore bring the box up **degraded**; 0.70.2 makes that visible and
  non-persistent instead of silent and permanent.
- **`conf` threshold is 60** — very high; real birds under 60% log unclassified.
  Likely an accidental NVS-wipe reset from ~17. User's call.
- **Don't spawn background monitors with a stray `&` inside the Bash tool** — it
  orphaned ~6 monitor processes this session that hammered the box at ~18 req/s and
  faked multi-second "stalls." Killing many keep-alive clients at once then caused
  ~90 s of TCP socket-pool recovery. Run one foreground command; verify no orphans
  with `Get-CimInstance Win32_Process`.
- **Cross-signed TLS gotchas** (from 0.70.0, still live): keep
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`; `certflags 0x0` does
  **not** mean a clean cert; esp-tls stores the mbedTLS code negated (print raw).
- **sdkconfig.defaults changes need `sdkconfig` deleted** to regenerate.

## What's left to do
- [ ] **Per-day visit-log files (KNOWN ISSUE, deferred — agreed to revisit).**
  The monthly `visits-YYYY-MM.csv` makes both gallery load and relabel SD-bound:
  a **~2.2 s floor on every `/api/events`** (reading the whole month — same cost
  for a 12-file or 2380-file day) + ~0.9 s readdir on a huge folder, and a **~6.5 s**
  monthly-CSV rewrite per relabel/batch. Splitting to `visits-YYYY-MM-DD.csv` fixes
  **both** (events → readdir floor; rewrite → ms). Larger change: touches the write
  path, `stats.c`, `h_labels_confirmed` (globs `visits-*.csv`), and
  `storage_reset_stats_day`. Candidate **0.70.5**. (Also in memory:
  `birdbox-per-day-log-files`.)
- [ ] **Batch the bulk Confirm path too** — still one `/api/confirm` per image
  (same class as the relabel fix, just wasn't the reported pain).
- [ ] **Add Anthropic API credit, then test cloud ID** (user, deferred) — set a
  spend cap, create a `birdbox` key, paste into Settings → Cloud Identification →
  🔌 Test connection (free), then ✨ on one photo. Leave the live toggle off.
- [ ] **Release the backlog** — 0.69.0 through 0.70.4 are all on master, none tagged.
- [ ] **Re-test OTA-from-URL** on current firmware — cross-signed roots on GitHub
  hosts mean §8 continuing to work depends on the CA bundle; `CROSS_SIGNED_VERIFY=y`
  makes it robust rather than lucky.
- [ ] **New/enhanced model** — clear classified images, load a new model, confirm
  the arena fits via `heapPsram`/`heapPsramBig`.
- [ ] **Consider restoring `conf` 60 → ~17** if the high threshold was a wipe reset.
- [ ] **v0.4 ROI-crop retrain** — the main open track; batch relabel now makes
  seeding the dataset much faster. Data collection is the bottleneck.

## Reference
- Device: **192.168.1.111** (confirm via `GET /api/status`). OTA (manual):
  `curl -s -X POST -H "Content-Type: application/octet-stream" --data-binary
  @build/BirdBox.bin http://192.168.1.111/ota/upload`.
- Build (PowerShell, from repo root): idf_tools.py env-export then `idf.py build`
  (export.ps1 broken — see CLAUDE.md). sdkconfig.defaults change → delete `sdkconfig`.
- Release: push tag `vX.Y.Z` → `release.yml` CI builds + publishes the esp32s3 bin.
- This session's commits: `d4a1f6f` (0.70.4 batch relabel + events trim),
  `be092c5` (0.70.3 species-ID handler offload), `6d4e4fe` (0.70.2 resolution
  degrade fix), `70cafd8` (0.70.1 gallery label cap → 6000).
- Prior: `f0692b2` (0.70.0 Claude cloud ID + TLS cross-signed fix), `25bc84d`
  (0.69.0 gallery species sub-filter).
