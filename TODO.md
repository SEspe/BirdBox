# BirdBox — TODO / backlog

Snapshot 2026-07-22 (fw 0.74.31). Nothing here is urgent — the box is healthy and
the cert-bundle DRAM leak (the ~107 min reboot cycle) is fixed by cert-pinning
(v2.64). Ordered by how much it actually matters.

## Added 2026-10-01 (1.0.2-1.1.0, the directory-scan work)
- [x] ~~**VERIFY: a bucketed read on a clean day folder.**~~ **DONE 2026-10-01
      on `.240`**, which after its power cycle had a fresh day folder (17 files,
      hour bucket `20/`): a bucketed read measured **0.023-0.039 s** against
      **~0.43 s** for the same request on `.205`'s 2650-file transitional
      folder. The predicted order-of-magnitude drop is real, and it reproduces
      on the OTHER unit and the other sensor. Caveat: 17 files is not a full
      hour - the steady-state case is a bucket holding ~500, which the model
      puts near 0.08 s and which still wants checking on a busy day.
      Original entry: The read-side win of v3.41 is PREDICTED, not measured. 2026-10-01 is transitional - its
      folder holds 2654 flat files AND the new hour directories, so resolving the
      directory `17` is itself a 2654-entry scan and a bucketed read still
      measures ~0.43 s. On 2026-10-02 the folder holds only ~24 bucket dirs, so
      the same read should fall to tens of ms. Test:
      `curl -o /dev/null -w "%{time_starttransfer}" http://192.168.10.205/captures/<tomorrow>/<a-frame>.jpg`
      (fetch it twice - the first call builds the per-day bucket mask).
      **If it does NOT fall, the O(N) model is wrong and v3.40/v3.41's whole
      rationale needs re-checking**, starting from the position-vs-latency
      measurement that produced 0.155 ms/entry.
- [ ] **Re-measure `capMs` over a full day.** It was the dominant unexplained
      cost; directory walks turn out to be ~5 s of it at ~6.8 frames/event.
      First post-change event read 7994 ms against 9641 / peak 18467 before, but
      that is one 5-frame event, not a day.
- [ ] **`append_visit_line()` still stats before appending** (storage.c). Same
      SHAPE as the v3.40 bug - `stat(path)` to decide whether to write the CSV
      header, then `fopen(path, "a")`, i.e. two path walks where one would do -
      but **nothing like the same size, and an earlier note here saying "~365
      entries, ~57 ms" was a guess and is wrong.** Measured context: `/log` holds
      one `visits-<date>.csv` and one `frameroi-<date>.csv` per day and there are
      15 days, so ~30 entries, i.e. ~5 ms for a FULL walk at the measured
      0.155 ms/entry - and the visits file normally exists, so the scan
      terminates early rather than running to the end. Call it 2-5 ms per event,
      ~390 events/day: a second or two a day. **Not worth a release of its own.**
      Fix when that file is next open: drop the `stat` and use
      `fseek(f, 0, SEEK_END); ftell(f) == 0` on the already-open append handle,
      which is cheaper and also immune to the stat/open race the current code
      has (harmless today only because the caller holds the write mutex).
      For contrast, `storage_log_frame_roi()` runs once per FRAME (~2647/day)
      and is already clean - a bare `fopen(..., "a")` with no stat.
- [ ] **The per-day bucket mask is single-entry.** Alternating between two days
      rebuilds it each time (measured: a bucketed read went 0.43 -> 1.33 s while
      interleaving two days). Real use is day-at-a-time so this has not bitten,
      but a 4-entry table would remove it if gallery browsing ever feels slow.
- [ ] **`.240`'s SD dropout is a LATCH that only a HARD POWER CYCLE clears.**
      ~~Earlier entry said the card was gone at the hardware level and should be
      replaced - WRONG, and corrected the same evening.~~ With the box on the
      bench and power-cycled, the card came back **completely healthy**:
      `sdCard:"BC2QT"`, 61039 MB total / 53317 MB free, and **every capture
      intact** - 53 day-folders back to 2026-07-08, including 2026-09-30's 1335
      files. Writes work (`POST /api/capture` 200). Nothing was lost.
      What this means: the fault survived `sd_recover()` (~589 failed writes,
      `sdRemounts` 0) AND a soft `/api/reboot` on 0.80.0 AND a fresh boot on
      1.1.0, but did NOT survive removing power. That is the **same failure
      class as the OV2640 camera latch** already in the notes - the peripheral
      wedges in a way a soft reset cannot reach, because the device is never
      actually de-powered. Treat "soft reboot did not fix it" as evidence FOR a
      latch, not against the hardware being fine.
      **OPEN QUESTION for the operator: was the card reseated, or only
      power-cycled?** Power-cycle-only means a true host/card latch and the
      remedy is firmware-visible (see the banner item below, plus a possible
      auto power-cycle of the SD rail if the board allows it). Reseat means
      mechanical contact and the remedy is physical. The two have different
      fixes and the answer is not recoverable from the device.
      Note this says NOTHING about `.205`'s card - the "degradation" there was
      the directory scan (v3.40), and `.205` reads a healthy ~2 MB/s.
- [ ] **The "SD WRITE FAILING" banner cannot fire when there is NO card** - and
      `.240` proves it cost 30 h of silent non-recording. `web_server.c:1479`
      toggles the banner on `s.sdWriteOk === false`, but `storage_save_jpeg()`
      returns `ESP_ERR_INVALID_STATE` on `!s_sd_present` BEFORE touching
      `s_last_write_ok` - so with the card absent the flag stays **true** and the
      one warning built for exactly this situation stays hidden. `.240` reported
      `sdWriteOk:true` while holding no card whatsoever.
      **Fifth instance of the same bug class** (counters that publish only on
      success - see v3.17/v3.18/v3.24/v3.28). Fix: drive the banner off
      `sdWriteOk === false || sdPresent === false`, and word it for both cases.
      Cheap, and it converts a 30-hour silent outage into an immediate one.
      Consider also a periodic re-mount attempt for a card that disappears
      mid-run: `sd_recover()` only runs on a failed write, and `sdRemounts:0`
      after 589 of them says that path is not recovering this failure anyway.

## Added 2026-09-29 (0.84.0-0.90.0)
- [ ] **`err:<reason>` has never fired in the field.** Shipped in 0.84.0; no iNat
      call has failed since. Until one does the tag is unwitnessed and the
      hour-08 error storm stays unexplained. This gates the framing work below.
- [ ] **15 "no bird" verdicts a day on weak evidence.** Per-frame scores like
      `Mollusca?=3`, `Animalia?=36` — iNat guessing non-Aves at 2-12% — file real
      events as background. §3.2 already requires BOTH a non-Aves top-1 AND no
      Aves anywhere (v2.45), so this is the threshold/framing question, not a
      rule bug. Same evidence gate.
- [ ] **The staleness banner cannot fire on a page that predates it (v3.22).**
      Dead code in exactly the case it was built for, and "no banner" was
      wrongly read as proof the page was current. Nothing server-side can fix a
      page that never re-fetches; the remedy is the cache-busting URL
      `http://<ip>/?v=2`. Worth considering: have the OTA success page link to
      `/?v=<version>` so the post-update visit is always a cache miss.
- [x] ~~**No live detector telemetry**~~ — **DONE 0.90.0 / FSD v3.24.**
      `/api/motion` now publishes `frames`, `loopMs`, `livePct`, `liveClust`,
      `liveCells`, `thr`, `gstep`, `decErr` on EVERY compared frame. Measured
      cadence at HD is **~400 ms** vs the 250 ms the loop requests.
- [ ] **Confirm detection in daylight after the QSXGA episode.** It fired once
      immediately at HD (17-cell cluster, 3% vs 2%), but the only sustained
      window was dusk. Check `/api/motion` `n` and `frames` mid-morning.
- [ ] **Characterise resolution vs detect cadence properly, now that it is
      measurable.** Read `loopMs` at HD / SXGA / UXGA / QSXGA and record the
      numbers in the FSD, replacing the inference that "larger is slower" with
      actual figures. QSXGA produced zero detections; HD is ~400 ms.
- [ ] **Re-check the queue peak at dawn.** Measured 4 of 16 (~148 s behind, zero
      drops) at midday; dawn is the busier window and is what would actually
      exercise the 16-deep buffer and the 15 s enqueue wait.
- [x] ~~**No GitHub release since v0.79.0**~~ — **DONE: v0.89.2 released
      2026-09-29**, asset `BirdBox_esp32s3_v0.89.2.bin`, verified byte-identical
      after download. `.240` is still on 0.80.0 and can take it from its own OTA
      tab (disable HA first).

## Blocked on evidence (added 2026-09-29, 0.84.0)
- [ ] **Do not touch `detect_zoom`, the detection zone or the framing until the
      `err:<reason>` tags have been read.** Operator call, and the right one:
      on 2026-09-29 **60% of unclassified events (39 of 65) were transport
      failures, not classifier misses** — the box never got an answer. Framing
      work aimed at the remaining 26 would be tuning against the smaller half
      of the problem, and if the storm turns out to be routine, today’s 51%
      identified is not the baseline any change should be measured against.
      **Gate:** wait for a day that logs `err:<reason>` (0.84.0 shipped the
      tag; nothing had failed yet when it was deployed), then split the losses
      transport-vs-classifier before proposing anything.
      Leading suspect to confirm or kill first: a **401 storm** —
      `inat_score()` refreshes the JWT at entry, and a flaky refresh fails
      every call until it takes, which fits both the ~50 min duration and the
      abrupt clean recovery. `err:401` in the log settles it either way.
      Context for when this unblocks: the genuine misses are **edge-clipped /
      peripheral birds against a seed-filled frame centre**, not a threshold
      problem, and `INAT_SOLO_ACCEPT_PCT` (75) should stay — see
      FSD changelog v2.33/v2.34 and v3.17.

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


## FIXED 2026-09-30 (0.99.0) — was: OTA panics while Home Assistant is enabled
- [x] ~~**`POST /ota/upload` panics the box whenever `haen=1`.**~~ **FIXED in
      0.99.0 / FSD v3.36.** Root cause: `MQTT_EVENT_CONNECTED` called
      `publish_discovery()` + `publish_state()` inside the esp-mqtt callback —
      on esp-mqtt’s own 6 kB task — with ~2.9 kB of buffers. Announcing now
      happens on `ha_task` (stack 6144→8192). **Re-verified 2/2 clean OTAs
      with `haen=1`**, `resetReason` `software` both times. The boot loop, the
      settings-save panic and the spontaneous one are all explained by the same
      marginal stack. Original report kept below for the reasoning.
- [x] ~~Reproducible A/B:~~
      **6/6 panics with HA enabled, 2/2 clean (`resetReason:"software"`) with
      `haen=0`.** Timing varies — usually after the image is written and the
      boot partition set (so the new firmware still boots), but at least once
      **mid-upload**, leaving the old image running. So it is NOT cosmetic.
      **Workaround, proven: `haen=0` → OTA → `haen=1`.**
      Three hypotheses were tested and are WRONG, do not re-try them:
      1. `species_refresh()` colliding with the flash write — guarded, still panics.
      2. The guard itself being broken (`web_server_ota_active()` was blind to
         `/ota/upload`) — genuinely broken and now fixed, but not the cause.
      3. MQTT activity during the write — `ha_suspend()` stopped the client
         before the first flash write and it STILL panicked. Reverted, because
         it cost up to 10 s on every upload for no benefit.
      Note what 1-3 rule out: it is not MQTT *activity* during the write, since
      `haen=0` differs by the ha task **never being created at all**. Suspect
      memory: internal DRAM is ~20-40 kB lower with HA up (~99 kB vs ~119-139 kB).
      **Next step needs a backtrace, and that needs hardware access** —
      core dump is `ENABLE_TO_NONE` and there is no coredump partition;
      adding one changes the partition table, which cannot be delivered by OTA.
      **New data point, 2026-09-29 (0.84.0):** the panic is not specific to
      OTA. Flipping `haen` 0→1 at runtime via `POST /api/settings` panicked
      the box on its own, with no upload in flight (`resetReason:"panic"`,
      uptime reset to 4 s). It recovered and has been stable since, and HA
      started cleanly on the following boot — so what panics is **creating
      the MQTT subsystem on an already-running system**, not the flash write
      it happened to coincide with. That fits hypothesis 1-3 being wrong and
      makes `ha_apply()`/`ha_start()` on a live box the place to look, which
      is far cheaper to reproduce than an OTA (one settings POST). Seen once,
      so not yet an A/B — confirm it repeats before acting on it.
      **2026-09-29, second trial: it did NOT repeat.** Same 0→1 flip on the
      same unit was clean, uptime continuous across the save. So the
      runtime enable is **intermittent (1 panic / 1 clean)**, not
      deterministic — which makes it a worse reproducer than the OTA path
      (6/6), not a better one. Do not build a bisect on it. What survives
      is the narrowing: a panic with no flash write in flight means the
      write is not necessary to trigger it.
      **Third data point, 2026-09-29 18:17:37:** `.205` panicked
      **spontaneously** ~3 h 45 min into an 0.89.2 boot with HA enabled — no
      OTA, no settings save, nothing in flight. `guardReboots:0`, so not the
      heap guard. That widens the fault from "creating the MQTT subsystem" to
      "running with it up at all", and suggests a cheap measurement that needs
      no backtrace: watch uptime/resetReason on `.205` with `haen=1` for a day,
      then with `haen=0` for a day, and compare. `.240` (HA never configured,
      3.4 days uptime) is already the control.

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
