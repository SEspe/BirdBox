# Session Notes — BirdBox (updated 2026-07-18)

Big session. Added **Google Gemini as a second, coequal cloud classifier**
(one active at a time), trained + deployed **nordic-v0.8**, added a **live-view
species overlay**, and root-caused + fixed **creeping web-UI sluggishness**.
Firmware went **0.70.13 → 0.72.1 / FSD v2.07 → v2.13**. The reference unit at
**192.168.1.111** now runs firmware **0.72.1** with the active model
**`nordic-v0.8.tflite`**. Cloud classifier is **configured (Gemini key stored,
verified working) but the Active provider is OFF** — no automatic cloud calls.
**TTA is off** (operator turned it off; halves inference to ~10 s).

(Prior sessions — three-tier cascade, nordic-v0.6, per-day visit log v2.07 — are
in git history + memory, not repeated here.)

## What shipped (commits on master, all built + deployed + verified)

- `1c5c422` **0.71.1 / v2.08** — **Gemini as a second cloud classifier.** Shared
  HTTP/JSON/TLS plumbing factored into `cloud_util.{c,h}`; `gemini.{c,h}` mirrors
  `claude.{c,h}`; `cloud.{c,h}` dispatches to the one active provider. Settings:
  `claude_enabled` bool → **`cloud_provider`** enum (0 off / 1 Claude / 2 Gemini);
  new `gemini_key`. NVS migrates old `s_cld` → `s_cprov`. Endpoints generalised
  to `/api/cloud-file` + `/api/cloud-test?p=<1|2>`; gallery gains a `[G]` badge.
- `10da626` **0.71.2 / v2.09** — auto-retry transient provider errors.
- `a10c808` **0.71.5 / v2.10** — Gemini reliability: model → `gemini-2.0-flash`,
  drop `thinkingConfig`, exponential backoff **2/4/8 s** on 429/5xx.
- `f3bc62b` **0.71.9 / v2.11** — **Gemini model is an operator-set Settings field**
  (`settings.gemini_model`, NVS `s_gmdl`, `[a-z0-9.-]` validated, in the export),
  default **`gemini-flash-lite-latest`**. Test Gemini lists callable model ids.
- `b77d8de` — `train.py` **MODEL_VERSION 0.8**; README registry rows for v0.7/v0.8.
- `8ff3c9f` **0.72.0 / v2.12** — **Live-view species overlay** (last event's
  species + confidence on the video; `classify_last_confidence()` → `spConf`).
- `6c0651c` **0.72.1 / v2.13** — **web-UI sluggishness fix + socket diagnostic**
  (see below).

## Gemini cloud classifier — the model saga (cost ~6 reflashes; important)

Gemini model availability shifts under you and **ListModels is misleading** (it
lists models `generateContent` then 404s). On this **new, billed** key:
- `gemini-2.5-flash` → 404 **"not available to new users"** — an account-tier
  gate that **enabling billing did NOT lift**.
- whole **2.0 generation** (`gemini-2.0-flash`/`-001`/`-lite`) → 404 **"no longer
  available"** (retired) even though ListModels still lists them.
- `gemini-flash-latest` → resolves but **sustained 503 "high demand"**.
- **`gemini-flash-lite-latest` → WORKS**: correct ID (Nøtteskrike @ 95%) in ~4 s.
  This is the default; change it in Settings (no reflash) if it ever 404s.

Other Gemini facts: **free tier is 10 RPM** — a no-billing key gets 429 "exceeded
quota"; billing is the fix (firmware can't manufacture quota). Body sends
`thinkingConfig.thinkingBudget=0` (usable flash models are thinking models; a
non-thinking model would 400 — but that generation is retired). Retry does 429 +
5xx with 2/4/8 s backoff, 4 attempts; other 4xx fail fast; a final failure
degrades to the on-device model. **Do NOT hammer the device with rapid cloud
calls** — overlapping TLS handshakes starve internal DRAM (~44 KB largest block
under load) and wedge the single httpd task.

## nordic-v0.8 retrain + deploy

- **Deployed ~20:15 on 2026-07-18** (uploaded + region-selected + rebooted; still
  firmware 0.71.9 at the time — model swaps never touch firmware). Active now.
- **95.0% val acc (n=262), up from v0.7's 91.9%.** The win: **Garrulus glandarius
  (Nøtteskrike)** went from a dead 0-image class (0% recall in v0.7) to a real
  trained class — **108 confirmed images → 80% recall/precision**. Dompap recall
  88→95%; precision up across the board (Lavskrike 64→91%, Bokfink 64→96%).
- Export pulled **684 new** confirmed images (5,563 already had). Per-class:
  Dompap 2563, no_bird 1777, **Lavskrike/Perisoreus_infaustus 722** (+2 in the
  free-typed `Lavskrike/` folder — folds into the same class), Skjære 513, Bokfink
  470, other 110, **Nøtteskrike 108**.
- Reminder: **Lavskrike = Perisoreus infaustus** (folders key on the Latin
  binomial, so Lavskrike confirmations land in `Perisoreus_infaustus/`).
- Still **ImageNet backbone** (iNat-init is a documented negative — see memory
  `inat-backbone-init-hurts`). `train.py` now MODEL_VERSION 0.8.

## Web-UI sluggishness — root cause + fix (v2.13)

Symptom: UI (stream, Gallery, every endpoint) grows sluggish over uptime; a
**reboot fully cures it** (happened twice). Profiled: warm requests **~20 ms**
(incl. the *cached* SD free-space query), **no memory leak** (2.12 MB at 50 min
vs 2.14 MB fresh), camera/WiFi healthy — but the **first request after idle
stalls ~5 s**, and 5 s == esp_http_server's default socket timeout. **Root cause:
socket-pool congestion** — default `max_open_sockets` 7 (capped by
`CONFIG_LWIP_MAX_SOCKETS`=10); a long-lived browser UI (keep-alive polls + the
jerky HD stream reconnecting) leaves slots in slow-to-reclaim states, so a fresh
request waits ~`recv_wait_timeout` (5 s) to purge one. **Fix:**
`CONFIG_LWIP_MAX_SOCKETS` 10→**16** (sdkconfig.defaults → clean rebuild),
`max_open_sockets` 7→**12**, `recv/send_wait_timeout` 5→**3 s**. **Diagnostic:**
`/api/sysinfo` `httpdSock`/`httpdSockMax` + Debug-panel **"HTTP sockets N / 12"**
(red near cap). Verified post-flash: 5/12, warm ~15 ms.

## Gotchas / decisions this session

- **Stream jerkiness at HD is inherent, not a bug** — ~130 KB/frame over WiFi =
  only a few fps. RSSI is strong (-55). Only knob is stream quality (operator's
  call; raise the number for smoother/lower-quality; it's the shared sensor JPEG
  setting so it also affects saved captures). Resolution stays HD (locked).
- **`/api/status` does a live SD free-space query**, but FATFS caches it after the
  first call, so it's ~20 ms — not a latency source. (Ruled out during profiling.)
- **Model deploy needs NO firmware update** — model is data on SD; upload +
  select + reboot loads it; the reboot is not a reflash. Firmware only matters if
  a model breaks the int8/op contract (`train.py` verifier catches that: PASS).
- **Don't hammer the device with back-to-back requests** while the operator is
  live-viewing — it competes for the httpd task + camera fb and worsens the feel.

## Known issues / deferred

- **frameroi sidecar not pruned** — `/log/frameroi-YYYY-MM-DD.csv` (0.70.9) grows
  append-only, not cleaned when old captures age out (small; FSD v2.03).
- **Socket-fix verification pending** — watch Debug "HTTP sockets N / 12" over a
  day: if it stays a low handful → root cause confirmed fixed; if it creeps toward
  12 → sockets are leaking somewhere and we chase the leak itself.
- **`recv/send_wait_timeout` cut to 3 s** — ample for LAN uploads, but a
  marginal-WiFi OTA/model upload has slightly less slack; bump back if uploads
  start failing.

## Device / next steps

- Device: 192.168.1.111, fw **0.72.1**, `nordic-v0.8.tflite`, **cloud Active
  provider OFF** (Gemini key stored + verified, model `gemini-flash-lite-latest`),
  **TTA off**, detect_zoom ON.
- **Watch over the coming day:** (1) UI responsiveness + the HTTP-sockets count
  (sluggishness fix); (2) nordic-v0.8 on live birds — confirm/correct misses in
  the Gallery to feed the next retrain; (3) the live-view species overlay.
- To enable live cloud ID later: Settings → Cloud Identification → Active provider
  → Google Gemini → Save (costs a fraction of a cent per escalated event).
- `training-data/` model artifacts + logs remain uncommitted by convention
  (dataset gitignored; only scripts/README tracked).
