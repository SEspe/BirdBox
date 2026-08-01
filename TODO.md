# BirdBox — TODO / backlog

Snapshot 2026-07-22 (fw 0.74.31). Nothing here is urgent — the box is healthy and
the cert-bundle DRAM leak (the ~107 min reboot cycle) is fixed by cert-pinning
(v2.64). Ordered by how much it actually matters.

## Do next (2026-08-01)
- [x] ~~**Switch the S3 target to QIO flash**~~ — **DONE, shipped 0.74.46 / FSD v2.80.**
      `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` in `sdkconfig.defaults.esp32s3`.
  - [x] ~~Verify the Boya reference unit on QIO~~ — `192.168.1.111` **serially reflashed**
        over COM12 and healthy. NVS + SD survived; the visit log is intact.
  - [x] ~~`flashMode` reports `"dio"` on a QIO build~~ — now reports the Kconfig booleans.
  - [x] ~~`.199` bootloader~~ — already QIO: it is the board that boot-loops under DIO, so
        the QIO bootloader written during the A/B is the only reason it runs. Both units
        are genuinely on QIO.
  - [ ] **BLOCKED (needs a cable):** re-test the second "dead" board
        (`28:84:85:65:68:f4`, still on stock `hello_world`) with a QIO build. Likely
        recoverable. **Both live units are network/OTA-only now**, so this and anything
        else bootloader-level (flash mode, PSRAM mode, partition layout) has to wait for
        physical access.

## Camera / sensor support (added 2026-08-01, 0.74.47-0.74.48)
- [ ] **`camera_af_error()`'s "AF firmware timed out (no VCM lens?)" branch is dead code.**
      `ov5640_af_init` collapses every failure to `-1` and `esp_camera_af_init` flattens
      that to `ESP_FAIL` (`esp_camera_af.c:56-58`), so the timeout case never reaches us.
      Reword to "AF firmware load failed (fixed-focus module?)" next time that file is open.
- [ ] **The OV5640 on `.199` is fixed-focus** — AF firmware load fails (`ESP_FAIL`), while
      SCCB is demonstrably healthy. No code fix; focus it by turning the lens thread.
- [ ] **Nothing has run above HD yet.** UXGA/QXGA/QSXGA are offered and clamped to the
      detected sensor, but no box has been booted at one — expect a slower detect loop and
      a narrower field of view, and watch `resActive` for a boot-time degrade.

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
