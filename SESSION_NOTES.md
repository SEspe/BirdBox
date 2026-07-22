# Session Notes — BirdBox (updated 2026-07-22)

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
