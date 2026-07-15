# Session Notes — BirdBox (updated 2026-07-15)

Rolling notes for the "BirdBox" work. Firmware is now **0.70.0**, FSD **v1.94**.
Everything committed and pushed to `origin/master`; **0.70.0 is live** on the
reference unit at **192.168.1.111**. GitHub **Releases v0.64.0 through v0.68.3**
are published (CI-built binaries attached) — **0.69.0 and 0.70.0 are not yet
released**, only pushed to master.

## Current device state (verified live)
- **version 0.70.0**, clock `ntp`, SD present.
- **Cloud identification (0.70.0) is installed but idle** — no API key stored
  (`ckey_set:false`, `cld:0`). The user will add Anthropic API credit and test
  later. Until a key is pasted in, nothing about the classify path changes.
- **Free internal DRAM ~181 KB** (largest block ~102 KB), **free PSRAM ~702 KB**
  (largest block ~704 KB) — steady even under camera+classify load (both run in
  PSRAM). These are the new `/api/sysinfo` fields (v1.87).
- **Resolution: HD 1280×720 (res=3)**, JPEG quality 8, `detect_zoom = 0`.
- **motion_sensitivity 76**, boot quarantine 30 s, cooldown 3 s, interval 1500, count 5.
- **`conf` (classify threshold) = 60** — very high (see gotchas). User's call.
- **Device address: 192.168.1.111** (confirmed live). CLAUDE.md's `192.168.10.236`
  is **stale** — always confirm with `GET /api/status`.

## What we did this session (0.64.0 → 0.70.0)

### 10. Cloud species ID via the Anthropic Claude API + TLS fix — 0.70.0, FSD v1.93/v1.94 (f0692b2), **not released**
- **Asked for:** identify birds with a cloud AI, on/off in Settings, to speed up
  labelling training images. Started on Google Gemini; the user switched vendors
  mid-build ("drop gemini, replace with claude.ai — same story, different AI
  vendor"), so the whole thing was ported to Anthropic (`claude-opus-4-8`).
- **Shape:** two independent gates. Settings toggle → each new motion event goes
  to Claude *instead of* the on-device model (one call per **event**, not per
  frame — best-of-N only exists to cover TFLM's pose sensitivity, which Claude
  doesn't share). Gallery **✨** button → one photo on demand, gated on a
  **stored key alone**, not the toggle, so you can label training images without
  turning on live billing. A failed call degrades to TFLM, never to a dropped
  event (§3.2).
- **Training-set safety:** the live path writes `species` (state 1), **not**
  `corrected` — an unattended cloud label can never reach `dataset/`. The ✨
  button *does* write `corrected` (mirrors 🔍 identify). Flagged to the user as
  a live design question; they kept it.
- **Model choice — settled:** user asked, decision was **keep Opus, don't
  downgrade to Haiku for cost.** Reason: the live label is what the human clicks
  ✓ on, so live errors *do* reach the training set laundered through a confirm
  click — and the errors that survive a glance are exactly the hard ones (Willow
  vs Marsh Tit). The cost lever is **call frequency** (toggle off, use ✨), not
  model quality.
- **Cost, and the subscription question:** ~1–2 US cents/event. A Claude Code
  subscription does **not** cover the API — `api.anthropic.com` needs separate
  pay-as-you-go credit from console.anthropic.com. User is adding credit later.
- **Opus 4.8 API constraints (bit me, will bite again):** `temperature`/`top_p`/
  `top_k`/`thinking.budget_tokens` are **removed → hard 400 if sent**. The ported
  Gemini body had `temperature:0` and would have failed every call. Determinism
  comes from the JSON output schema (`output_config.format`), not temperature.
  `thinking` omitted = no extended thinking, which is right for a single-image ID.
- **Verified on hardware:** build clean, served JS balanced, all handlers present,
  key absent from `/api/settings` and the export. End-to-end, ✨ read a real SD
  JPEG, streamed it to Anthropic, and parsed the reply — **401 only because the
  key was a placeholder.** Every layer works; only a real key is missing.

### 9. Gallery: filter confirmed birds by a single species — 0.69.0, FSD v1.92 (25bc84d), **not released**
- (Prior session; recorded here because SESSION_NOTES was never updated for it.)
- Species sub-filter (`#gspecf`) appears when Show = Confirmed (bird), populated
  from the distinct confirmed species on the loaded day; narrows grid + tally.
  Pure client-side, no new route or API field.

### 8. Gallery thumbnails match the 16:9 capture aspect — 0.68.3, FSD v1.91 (5707e97), Released
- **Reported (user):** thumbnails cropped in width — 16:9 captures forced into 4:3 tiles.
- **Cause:** `.gitem img{aspect-ratio:4/3;object-fit:cover}` — stale default from
  before HD 16:9 was locked (v1.80). A 16:9 frame covering a 4:3 box loses its
  left/right edges.
- **Fix:** tile box → `aspect-ratio:16/9`, so `cover` has nothing to crop; full
  frame width shows. One-line CSS. (Legacy 4:3/SXGA-5:4 now crop slightly
  top/bottom instead — fine, HD 16:9 is the shipped default.)
- **Note:** flashed 0.68.3 to the device *before* the user said "no OTA" (the
  upload had already returned OK) — verified live, then held commit/push/release
  until the user asked. No second OTA performed.

### 7. Gallery toolbar + tally layout cleanup — 0.68.2, FSD v1.90 (b550652), Released
- **Reported (user):** toolbar/tally confusing; buttons different sizes; tally
  wrapped one word per line ("961 / captures / · 504 / confirmed / …").
- **Cause:** the toolbar was one non-wrapping flex row holding day-select + 7
  buttons + filter + the counts span; on a narrow window buttons shrank to their
  text (unequal) and the counts span got squeezed into a one-word column.
- **Fix (UI-only):** action buttons moved to a responsive grid (`.gacts`,
  `repeat(auto-fill,minmax(160px,1fr))`) so all are **equal size** and wrap into
  tidy rows; counts render as an **aligned label/value table** (`renderTally()`
  → `.gtallytab`, right-aligned tabular-nums) instead of a `·`-joined
  sentence; filter hints (double-click, near-threshold band) drop to a caption
  line. Two Recheck labels shortened to keep the grid compact.

## What we did this session (0.64.0 → 0.68.1)

### 6. Fix Gallery multiselect delete truncating large selections — 0.68.1, FSD v1.89 (4aeb7c7), Released
- **Reported:** selecting all 285 unclassified frames on a day and hitting
  **Delete selected** didn't delete them all.
- **Root cause:** `h_captures_delete_batch` (`POST /api/captures/delete`) read the
  body with a **single** `httpd_req_recv()` — the one batch handler that *didn't*
  loop. The multiselect list is one `files=a.jpg,b.jpg,...` body (~8 KB for 285
  names) that TCP splits across segments; the single recv got only the first
  ~1.4 KB, so ~50 files deleted and the rest were silently dropped (JS ignored the
  returned `deleted` count).
- **Fix:** loop `httpd_req_recv` until the whole body is read (up to the 16 KB cap
  ≈ 560 files), matching the sibling handlers (`/api/relabel`, `/api/confirm`,
  `/ota/from-url`); `gDelSel()` now compares `deleted` vs requested and alerts
  *"Deleted N of M — reload and delete the rest"* so a truncation can never pass
  silently again. Whole-day **Delete all**/**Wipe day** (`all=1`) were never
  affected (no file list).
- **Verified:** user re-ran the 285-frame delete on 2026-07-13 — all removed OK.


### 1. Gallery bulk-label ops + Maintenance tab — 0.64.0, FSD v1.84 (a9eed9a), Released
- **✔ Confirm selected** (loops `/api/confirm`) and **🔖 Set species▾** (inline
  datalist picker, loops `/api/relabel`, seeds copy-last) — selection-scoped bulk
  labelling. Both write the visit-log `corrected` column → feed the retrain export.
- Requests issued **strictly sequentially** (`gSeq`) — the ESP32 httpd has only a
  handful of worker sockets, so never a fan-out. **No new firmware routes** (reuses
  the per-image handlers).
- **New "Maint" tab** — moved the whole-day destructive ops (**Delete all photos**,
  **Wipe day**) out of the Gallery bar (mis-click risk next to labelling) into their
  own tab with a day selector. UI-only.

### 2. Motion false-positive fix — global-illumination compensation — 0.65.0, FSD v1.85 (1a88237), Released
- **Reported:** motion firing on the empty feeder, several *exactly when the live
  view was opened*. Pulled the frames — no bird, just a whole-frame brightness swing
  (auto-exposure re-converge when `/stream` starts; motion runs on the *same single
  sensor stream*).
- **Root cause:** `motion.c` used a fixed absolute per-pixel threshold with no global
  compensation; the dual-EMA + 20-cell cap only stop *gradual*/*uniform* changes, so
  a sudden non-uniform AE swing (bright stump clips, dark shed doesn't) forms a
  bird-sized cluster and triggers.
- **Fix:** subtract each background's frame-mean brightness delta before thresholding
  (uniform step cancels; a local bird still stands out — it shifts the mean only
  ~1-3 levels). Guard: a frame whose mean shift exceeds `GLOBAL_STEP_THR = 12` can't
  trigger; EMA re-seeds so detection recovers in ~1 s. Zone/sensitivity untouched.
- **Verified:** reproduction (opening `/stream` repeatedly) → 0 new captures.

### 3. Progress bar for bulk-label ops — 0.66.0, FSD v1.86 (cba0090), Released
- Determinate bar below the gallery toolbar ("Confirming – 34 / 120 (28%)" /
  "Setting species – …"), updated per image, auto-hides ~0.5 s after finishing.
  UI-only; no change to the sequential pattern.

### 4. Heap diagnostic: internal DRAM + PSRAM reported separately — 0.67.0, FSD v1.87 (f1fca8b), committed (not released)
- `esp_get_free_heap_size()` lumps both pools; added `heapInt`/`heapIntBig` and
  `heapPsram`/`heapPsramBig` (free + largest contiguous block per pool) to
  `/api/sysinfo` and the Debug tab. Groundwork for the two memory questions below.
- **Measured:** ~180 KB internal DRAM free (100 KB largest block), steady under load;
  ~705 KB PSRAM free. This is what made the OTA-from-URL memory call a clear "yes."

### 5. Flash a GitHub release directly from the OTA tab — 0.68.0, FSD v1.88 (b351fed), Released
- OTA tab lists this repo's releases (all of them, running one marked) and flashes a
  chosen one over the air, with a progress bar. **No PC/cable.**
- **Why device-side download:** the GitHub *API* sends CORS `*` (browser can list),
  but the release *asset* redirects to `release-assets.githubusercontent.com` with
  **no CORS header** (browser can't read the bytes). So: browser fetches the release
  list + POSTs the picked asset URL to **`POST /ota/from-url`**; the device downloads
  +flashes via `esp_https_ota` in a background task; **`GET /ota/from-url`** reports
  `{state,read,total,msg}` for the poll.
- **Security — repo-locked:** rejects any URL not starting with
  `https://github.com/SEspe/BirdBox/releases/download/`. Reuses dual-partition rollback.
- **Build:** adds `esp_https_ota`/`esp_http_client`/`mbedtls` to `main`, enables
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` + `DYNAMIC_BUFFER` in `sdkconfig.defaults`
  (clean rebuild required; +140 KB flash → 65% partition free).
- **Verified end-to-end:** device self-downloaded v0.68.0 from GitHub (1,476,624 B,
  100%, ~21 s) through the redirect + cert-bundle validation, rebooted healthy on
  0.68.0. Repo-lock rejected `evil.example.com` and a wrong-repo URL. Internal DRAM
  unchanged post-flash (~181 KB) — cert bundle is flash, not heap.
- **Still true after the 0.70.0 TLS work** — this record is what disproved a claim I
  drafted in FSD v1.93 that outbound HTTPS "never worked against any host" and that
  this path was broken. It wasn't: its chain terminated at a root the bundle holds
  directly, so it never needed the cross-signed hop. Only chains that *need* that hop
  failed. **Caveat for later:** as of 2026-07-15 `github.com` (Sectigo Root E46
  cross-signed by USERTrust ECC) and `release-assets.githubusercontent.com` (ISRG Root
  YR cross-signed by ISRG Root X1) now serve cross-signed roots too, so §8 continuing
  to work depends on which roots the bundle holds as CAs rotate. `CROSS_SIGNED_VERIFY=y`
  makes it robust rather than lucky — worth an OTA-from-URL re-test next session.

## Memory evaluation (settled this session — see v1.87/v1.88)
- **Camera + classifier run entirely in PSRAM**, so internal DRAM sits at ~180 KB
  free (100 KB largest block) and barely moves under load.
- **TLS/OTA-from-URL:** peak ~40-60 KB internal → ~120 KB headroom. Comfortable.
  `DYNAMIC_BUFFER` shrinks the peak further. TLS buffers left in internal DRAM
  (proven fine); PSRAM-routing was considered but unnecessary.
- **New model (tomorrow):** the TFLM arena is a single big **PSRAM** alloc; the
  ceiling is `heapPsramBig` ≈ **704 KB** contiguous above the current model. Load the
  new model and read `heapPsram`/`heapPsramBig` before/after — the drop *is* the
  arena; a failed alloc shows immediately in the Debug tab.

## Key decisions
- Bulk-label ops **sequential, never fanned out** (tiny httpd socket pool); progress
  bar makes the wait tolerable. Reuse existing endpoints (no route-table growth).
- Destructive day-ops isolated in the Maintenance tab.
- Global-illumination compensation is an **algorithmic robustness fix**, separate
  from zone/sensitivity (operator-owned, untouched). A zone change wouldn't fix an
  AE swing — it hits every cell. `GLOBAL_STEP_THR=12` set conservatively to protect
  bird sensitivity.
- OTA-from-URL is **repo-locked** (can only flash SEspe/BirdBox signed releases) and
  reuses rollback — a bad image reverts on next boot.
- Route table now **44/56** (cap raised from 48 in 0.70.0 for the two `/api/claude-*`
  routes) — registrations past the cap are **silently dropped** and the endpoint 404s
  while the handler code looks fine.
- **Keep Opus for cloud ID; don't downgrade for cost** — see §10. Cost is controlled
  by *how often you call*, not by model quality.

## Gotchas (still live)
- **`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY` must stay `y`** (0.70.0).
  It defaults to **n**, and without it `esp_crt_bundle` cannot validate a chain whose
  trust path runs through a **cross-signed root** — `api.anthropic.com` serves a GTS
  Root R4 cross-signed by GlobalSign. Don't "clean it up" when changing API vendors:
  anything behind Google Trust Services has the same shape.
- **`certflags 0x00000000` does NOT mean the certificate verified cleanly.** The
  cross-signed failure returns `MBEDTLS_ERR_X509_FATAL_ERROR` (**-0x3000**) *without
  setting any verify flag*, so it reads exactly like a clean chain and sends you
  hunting the network instead of the trust store. This misread cost a whole debugging
  pass. Also: esp-tls stores the mbedTLS code **negated** — print it raw, or you get
  `0xFFFFD000` and think it's garbage.
- **Don't bother writing a "retry with TLS verification off" A/B probe** — one was
  written and removed. `CONFIG_ESP_TLS_INSECURE=n` makes esp-tls refuse TLS setup with
  no CA at all, so the probe fails at *setup*, proves nothing, and its "fails both
  ways" verdict is actively misleading.
- **`conf` threshold is 60** — very high; real birds under 60% log as *unclassified*.
  Likely an NVS-wipe reset from the tuned ~17. Restoring it is the user's call.
- **Detection zone is permissive** — bottom row (foreground grass) + right-side tree
  line are active wind-driven false-positive sources. Tightening helps *wind*
  triggers (not AE ones). **User owns the zone** — explain, don't apply uninvited.
- **Motion shares the single sensor stream** — opening live view perturbs AE (that's
  what 0.65.0 fixes). Watch for any residual localized-clip false positives; if one
  sneaks through, lower `GLOBAL_STEP_THR` from 12 (with live evidence).
- **Whole UI is one inline `<script>`** — one JS syntax error kills all handlers; the
  C build can't catch it. Grep the served page + bracket/ternary balance pass.
- **sdkconfig.defaults changes need `sdkconfig` deleted** to regenerate (done for
  0.68.0). **Build must run from the repo root** (deletes any stray `main/build`).
- **Boot quarantine (30 s)** and **still-bird-absorbed-into-background** both look
  like dead detection but aren't.

## What's left to do
- [ ] **Add Anthropic API credit, then test cloud ID** (user, deferred) —
  console.anthropic.com → load credit → set a spend cap → Settings → API Keys →
  create a `birdbox` key. Paste into Settings → Cloud Identification → **🔌 Test
  connection** (free, validates against `/v1/models`, spends no tokens). Then try
  **✨** on one Gallery photo — that's the first billed call. Leave the live toggle
  **off** unless actively gathering labels.
- [ ] **Release 0.69.0 + 0.70.0** — both are on master but no tag/CI release yet.
- [ ] **Re-test OTA-from-URL** on 0.70.0 — see the cross-signed caveat under §5.
- [ ] **New/enhanced model (tomorrow's plan)** — user will clear classified images
  then try a new model. Use the new PSRAM/`heapPsramBig` readout to confirm the arena
  fits; the OTA-from-URL feature lets firmware roll fwd/back easily while iterating.
- [ ] **Watch motion false-positive rate** — lower `GLOBAL_STEP_THR` only with live
  evidence a localized clip-swing still triggers.
- [ ] **(Optional) Tighten detection zone** to the stump top to cut wind/grass false
  positives — user's call (paint in Live → zone editor, or ask me to push a mask).
- [ ] **Consider restoring `conf` 60 → ~17** (tuned value) if the high threshold was
  an accidental wipe reset.
- [ ] **Confirm bulk-op + OTA progress bars** fill smoothly with real use in the browser.
- [ ] **v0.4 ROI-crop retrain** — the main open track (ROI logged since 0.60.0; the
  new bulk-label ops speed up seeding the dataset). Data collection is the bottleneck.
- [ ] `bugreport.txt` is the running bug tracker (untracked); bug 1 fixed in 0.63.0.

## Reference
- Device: **192.168.1.111** (confirm via `GET /api/status`; CLAUDE.md's `.10.236` is
  stale). OTA (manual): `curl -s -X POST -H "Content-Type: application/octet-stream"
  --data-binary @build/BirdBox.bin http://192.168.1.111/ota/upload`. OTA (from GitHub):
  OTA tab → pick release → Download & flash (or `POST /ota/from-url` with the asset URL).
- Build (PowerShell, from repo root): idf_tools.py env-export then `idf.py build`
  (export.ps1 broken — see CLAUDE.md). sdkconfig.defaults change → delete `sdkconfig` first.
- Release: push tag `vX.Y.Z` → `release.yml` CI builds + publishes with the esp32s3 bin.
- Latest commits: `f0692b2` (0.70.0 Claude cloud ID + TLS cross-signed fix),
  `25bc84d` (0.69.0 gallery species sub-filter), `5707e97` (0.68.3 16:9 thumbnails), `b550652` (0.68.2 toolbar
  + tally layout), `4aeb7c7` (0.68.1 delete recv-loop fix), `b351fed` (0.68.0
  OTA-from-URL), `f1fca8b` (0.67.0 heap split), `cba0090` (0.66.0 progress bar),
  `1a88237` (0.65.0 illum comp), `a9eed9a` (0.64.0 bulk ops + Maint tab).
