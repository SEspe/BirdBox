# BirdBox — TODO / backlog

Snapshot 2026-07-22 (fw 0.74.31). Nothing here is urgent — the box is healthy and
the cert-bundle DRAM leak (the ~107 min reboot cycle) is fixed by cert-pinning
(v2.64). Ordered by how much it actually matters.

## Real bugs (small, located)
- [x] ~~`gemini.c` handle leak (3 init vs 2 cleanup)~~ — **INVESTIGATED, NOT A LEAK
      (2026-07-22).** The "3rd init" was a grep false positive: line 174 is a *comment*
      containing the text "esp_http_client_init". There are 2 real `esp_http_client_init`
      calls and both functions (`gemini_post_once`, `gemini_models`) pair init→cleanup on
      every path — `if (!c) return` (NULL handle, nothing to free) and all error paths
      `goto out` → close + cleanup. No fix needed.
- [ ] **Cloud tier still uses the cert bundle** (claude.c / gemini.c) — so it still hits
      the leaky CA callback. Barely matters (infrequent, off by default); pin its roots
      the same way as iNat if cloud ever becomes the heavily-used active provider.
      (Cloud CAs differ from iNat's — needs its own root set.)

## Watch (no code — passive verification)
- [ ] **Confirm the reboot cycle stays dead** — a full day of uptime with `guardReboots`
      steady (was 3) and `heapIntBig8` holding ~31744. Already ~confirmed at 114 min.
- [ ] **`capture_count=3` experiment** — does classification now stay inside the 120 s
      cooldown, and do more single-good-frame birds slip to unclassified? If the latter,
      3 was a touch too few (bump toward 4).

## Optional / nice-to-have
- [ ] **frameroi sidecar pruning** — `/log/frameroi-*.csv` grows append-only, not cleaned
      as captures age out. Small/cosmetic (deferred since v2.03).
- [ ] **Camera framing** (placement, no code) — edge-clipped birds are iNat's blind spot;
      aiming so birds aren't cut off at the frame edge converts a class of misID/"no bird"
      into clean IDs.
- [ ] **`cooldown_s` = 120 s is long** — a second bird arriving mid-cooldown is ignored.
      Lower it if follow-up visitors are being missed (trade: more events / iNat calls).
- [ ] **Per-event latency** (~10 s/frame, inherent to iNat-online). Levers if it bothers:
      fewer frames, lower solo-accept bar, or the cloud tier (~4 s/call). All tuning calls,
      left to the operator.

## Settled — DO NOT re-attempt (documented dead ends)
- Keep-alive TLS reuse — doubled classification time (~9.6→16.8 s), reverted (v2.62).
- VGA / lower-res fast frames — no speedup; OV2640 ~1 fps @ 20 MHz XCLK is a hardware
  floor. The nominal 200 ms fast-burst gap is unreachable on this sensor.
- OAuth for iNat login — needs a registered app (~2-month approval); form-login is the
  deliberate workaround.
