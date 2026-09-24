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

## View rotation / mirroring (added 2026-08-01, 0.74.49)
- [ ] **The `s_rot` → `s_rotd` migration only ran its degenerate case.** Both units were at
      rotation 0, so the fallback branch executed and correctly produced 0°, but the `× 90`
      conversion never had a non-zero input on real hardware. Check any box that was set to
      90/180/270 before upgrading past 0.74.49.
- [ ] **Species ID still reads the UNROTATED frame.** Fine for a 2-3° levelling tweak; if a
      camera ends up genuinely mounted at 90°, iNat sees a sideways bird. Fixing it means a
      full-frame decode + JPEG re-encode per event — the same trade already declined for
      captures, but it's one frame per event there, not five, so it's cheaper than it looks.
- [ ] Saved JPEGs keep the sensor orientation at every angle **except exactly 180°**, which
      the sensor does itself. Deliberate (operator's call), but it means the training
      pipeline and Windows Photos see un-levelled images. EXIF Orientation would fix the
      quarter turns cheaply if that ever matters — note PIL ignores EXIF unless train.py
      calls `ImageOps.exif_transpose`.

## Camera / sensor support (added 2026-08-01, 0.74.47-0.74.48)
- [ ] **`camera_af_error()`'s "AF firmware timed out (no VCM lens?)" branch is dead code.**
      `ov5640_af_init` collapses every failure to `-1` and `esp_camera_af_init` flattens
      that to `ESP_FAIL` (`esp_camera_af.c:56-58`), so the timeout case never reaches us.
      Reword to "AF firmware load failed (fixed-focus module?)" next time that file is open.
- [ ] **The OV5640 on `.199` is fixed-focus** — AF firmware load fails (`ESP_FAIL`), while
      SCCB is demonstrably healthy. No code fix; focus it by turning the lens thread.
- [x] ~~**Nothing has run above HD yet.**~~ **EXERCISED 2026-09-23 on `.205`:** UXGA and
      QXGA both boot and run, `resActive` matches the request, no boot-time degrade. Cost
      is thermal and it is large — see the temperature table under "Temperature alert +
      throttling" below (~12 °C for UXGA, ~17 °C for QXGA over HD). Reverted to HD, which
      remains the right default. Detection above HD was NOT characterised: `.205` logged 0
      events during its hours at UXGA/QXGA, but it is a bench unit with little traffic, so
      that is not evidence either way. Original note: UXGA/QXGA/QSXGA are offered and clamped to the
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


## Found 2026-09-22, not yet fixed
- [x] ~~**No JSON emitter checks its `snprintf` return.**~~ **FIXED 0.78.4 /
      FSD v3.11** — and it was worse than "malformed JSON": eight sites passed
      the unclamped return straight to `httpd_resp_send()` (out-of-bounds READ,
      shipping adjacent memory to an unauthenticated LAN client) and seven more
      in `ha.c` accumulated with `sizeof(buf) - n`, which underflows `size_t`
      into an enormous length (out-of-bounds WRITE). See `json_fit()` and
      `jcat()`.
      `web_server.c` and `ha.c`, none guarded. A truncated reply is silently
      malformed: it takes down the entire Settings tab, or turns all 29 Home
      Assistant entities "unknown" at once, and the firmware cannot tell you it
      happened. Four buffers were grown by hand in one week purely by eyeball.
      One `if (n >= (int) sizeof(buf)) ESP_LOGE(...)` per emitter converts a
      silent failure class into a loud one.
- [ ] **`classify.cpp` last-species strings are read unsynchronised.**
      `s_last_species`/`s_last_latin` are written by the event task and read by
      HTTP handlers; a torn read is possible. Cosmetic, but real.

- [ ] **`/api/motion` `rej` and `rejN` are named backwards from what they mean.**
      `rej` is the largest rejected cluster SIZE, `rejN` is the COUNT — easy to
      misread, and it was misread once. The HA entities (v3.09) use clear names
      (`rejected`, `rejected_max`); renaming the JSON fields would break the
      live-view overlay that consumes them, so it needs the UI updated in the
      same change.

- [ ] **The boot quarantine skips ambient sampling.** `motion_task()` takes the
      pause branch first, then `continue`s on the quarantine branch BEFORE reaching
      `decode_gray()`, so the box is blind to dark/bright for `detect_quarantine_s`
      after every boot. Benign at the 60 s default (it self-clears), but the setting
      accepts up to **3600** and an hour-long quarantine would blind night-sleep
      detection for that hour. Same root cause as FSD v3.05, same two-line fix:
      sample ambient in the quarantine branch too. Left unflashed because it was
      found after the overnight sleep test had already started.

## New functionality — candidates (added 2026-09-22, fw 0.76.0)

- [ ] **Temperature alert + throttling.** Today the SoC temperature is *reported only*:
      the Debug row turns red above 75 °C (`web_server.c`), `/api/sysinfo` carries
      `socTempC`, and 0.76.0 publishes it to Home Assistant as a `temperature` sensor.
      **Nothing acts on it.** Two separate pieces, worth doing in this order:
  - **Alert** is nearly free now. Home Assistant can alarm off the published sensor with
    *zero* firmware work — that alone may close the request. On-device, an "overheating"
    binary sensor is one row in `ha.c`'s `ENTITIES[]` table plus a Debug banner.
  - **Throttling** needs care about *which* lever. Cheapest first: raise `cooldown_s`,
    cut the fast-burst frame count, stop serving the MJPEG stream (continuous streaming is
    the heaviest sustained load), drop the illuminator. **Resolution is NOT a runtime
    lever** — it is applied at `camera_init` and needs a reboot (`settings.h`), so a
    thermal path cannot step it down without restarting the box. Needs hysteresis
    (act at ~80 °C, release at ~70 °C) or it will oscillate at the threshold.
  - **Reference numbers — read the caveats, they matter.**

    **ESTABLISHED (same box, short window, control unmoved):**
    - **One attached Live-tab viewer ≈ +14 °C.** Seen as a *step*, not a ramp,
      which is how it was told apart from sun: the control box did not move at
      that moment. Confirmed twice. **This is the most effective lever a
      throttler could pull, and unlike resolution it CAN be pulled at runtime.**
    - **Camera powered down overnight ≈ −10 °C** (§14). `.205` ran 38 °C asleep
      against 51–53 °C awake; ~4 °C of that 14 °C drop was ambient cooling,
      measured on the control, leaving ~10 °C for the camera itself.

    **NOT ESTABLISHED — earlier claims of "~12 °C for UXGA, ~17 °C for QXGA over
    HD" are RETRACTED.** Those came from readings taken at different times of
    day, with different stream states, and none at thermal steady state (the
    QXGA figure was 5.6 min after boot; the HD figure was still climbing when it
    was recorded). Worse, they compared `.205` against `.240` — a bench unit
    indoors against a box outdoors in September, so most of the gap is **ambient,
    not resolution**. Observed spread at HD alone: `.240` 40 °C outdoors,
    `.205` 63 °C indoors with a viewer attached.

    **To actually measure resolution cost:** one box, Live tab closed, ≥20 min
    settling at each resolution, same afternoon, A/B. Not yet run.
  - Remember the sensor reads the **die**, not enclosure air (typically 20–30 °C above
    ambient). The genuine risk is not the chip — it is a sealed box in direct summer sun,
    where the same workload lands far higher. Shade beats any firmware lever here.

- [x] ~~**Daylight sleep**~~ — **SHIPPED 2026-09-22 as 0.77.0 / FSD v3.04** (fix in 0.77.1 /
      v3.05). Built on the camera's own ambient reading, three levels, timed wake probe.
      Deep sleep (level 2) is implemented but NOT yet exercised on hardware. The notes
      below are kept as the design rationale that produced it.
- [ ] ~~Daylight sleep, original entry:~~ No birds at
      night, so capture, classification, iNat calls, SD writes and the illuminator are all
      wasted, along with the heat they make. **Everything needed to compute sunrise/sunset
      on-device already exists**: NTP time, `g_settings.timezone`, and a latitude/longitude
      — `inat_loc` already stores `"lat,lng"` for the iNat geo hint. A dedicated lat/lng
      setting would be cleaner than borrowing that field, but nothing new is *required*.
  - **Decide what "sleep" means first — the two options are not close.** Operator's call
    (2026-09-22): make the depth **optional, chosen to suit the installation** — mains and
    battery want opposite answers. So a three-way setting rather than a boolean:
    **off** (default) / **suspend detection** / **deep sleep**.
    *(a) Suspend detection only.* `motion_set_detection_enabled(false)` already exists and
    is already exposed at `/api/detect`, so this is mostly scheduling. The web UI, stream,
    OTA and HA reporting all stay alive. Low risk, and it still stops the SD writes, iNat
    calls and illuminator — most of the actual waste. The right answer on mains, where the
    only thing saved is heat and SD wear and reachability is worth more.
    *(b) Real deep sleep.* Much bigger saving, but the box **disappears from the LAN**: no
    web UI, no OTA, no HA telemetry until it wakes. On a mains-powered box that is a bad
    trade; **on battery or solar it is the entire point**, and it is the mode that makes a
    battery deployment viable at all. Explicit opt-in, never silent.
  - **The box cannot tell whether it is on battery — and that matters here.** There is no
    power-source sensing in the firmware and no battery monitor wired on either board
    (`board_config.h` has no ADC/voltage-divider pin, and `/api/sysinfo` reports no supply
    rail). So "deep sleep when on battery" has to be an **operator declaration** — the
    three-way setting above — not an automatic decision. Making it automatic means new
    hardware: a divider into an ADC pin, which would also be worth having in its own right
    (battery percentage is an obvious HA sensor, and the `.205` unit's power dropouts would
    have been diagnosed in minutes rather than hours had the box been able to see its own
    supply). Treat "battery voltage sensing" as the prerequisite feature, not part of this.
  - **A battery deployment changes assumptions beyond sleeping.** While asleep the box
    cannot be OTA'd, so an update has to wait for a wake window; HA will show it
    unavailable every night (the last-will availability topic makes that correct rather
    than alarming, which is one reason it was built that way); and NTP resync, WiFi
    reconnect and camera warm-up all get paid again at every wake. Worth costing before
    assuming deep sleep is a pure win.
  - **Guard the pre-SNTP clock.** Right after boot `clockSrc` is not yet `ntp` and the
    clock reads ~1970 — computing an almanac against that yields a bogus "it is night" and
    a box that silently refuses to detect. Same bug class the boot detection quarantine
    already exists for. Gate on `clockSrc == ntp` and fail *open* (keep detecting) when the
    time is not trusted.
  - **Consider the simpler trigger.** The illuminator already decides day/night from how
    dark the camera's own frames read (`motion.c` ambient check). Reusing that needs no
    location, no almanac and no clock at all, and it self-corrects for a shaded or
    north-facing site that an almanac would get wrong. Probably the more robust option —
    an almanac is the obvious design, not necessarily the right one.
  - Needs a manual override, so someone checking the box at night is not locked out; and
    it should be a setting defaulting **off**, since a nest box (unlike a feeder) may be
    worth watching after dark.

## Settled — DO NOT re-attempt (documented dead ends)
- Keep-alive TLS reuse — doubled classification time (~9.6→16.8 s), reverted (v2.62).
- VGA / lower-res fast frames — no speedup; OV2640 ~1 fps @ 20 MHz XCLK is a hardware
  floor. The nominal 200 ms fast-burst gap is unreachable on this sensor.
- OAuth for iNat login — needs a registered app (~2-month approval); form-login is the
  deliberate workaround.
