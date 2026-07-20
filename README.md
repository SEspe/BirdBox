# BirdBox

Open-source WiFi nest box / bird feeder camera on an **ESP32-S3**, built with
**ESP-IDF** (no Arduino, no cloud). It detects bird visits, captures photos to
microSD, identifies the species via iNaturalist's online AI, and serves a live
stream, gallery and visit statistics from its own built-in web page — entirely
on your LAN.

**Full specification:** [FSD_BirdBox.md](FSD_BirdBox.md) — the FSD is the
project's requirements document *and* change record; every functional change
gets a changelog entry there.

> **Status: v1 functionally complete.** WiFi provisioning, live streaming,
> microSD storage + retention pruning, motion-triggered capture, on-device
> species ID, and the full seven-tab web UI (Live, Gallery, Stats, Settings,
> Debug, WiFi, OTA Update) are all implemented and verified live on the
> reference hardware. See the [FSD changelog](FSD_BirdBox.md) for the
> complete history.

## Features

- **Live MJPEG stream** in the browser (or any player, e.g. VLC/Home Assistant),
  with a "stream busy" message + auto-reconnect when the 2 client slots are full.
- **Motion-triggered capture** to microSD, with configurable sensitivity,
  follow-up frame count, cool-down, and a **boot detection quarantine** that
  suppresses false triggers from the camera's warm-up frames / not-yet-synced
  clock after a reboot.
- **Online species identification** — each visit is identified by
  **iNaturalist's online Computer Vision API** (the model that powers the iNat
  app): current, region-aware, and free. BirdBox scores every saved frame,
  requires a **≥2-frame corroboration** (a single confident frame is accepted
  only above a high bar), and applies a **Norway allowlist** so a
  geographically-impossible guess (e.g. the American magpie *Pica hudsonia*) is
  rejected in favour of the right local species (*Pica pica*). It also sends a
  native-res **crop of the motion box** when the whole frame under-scores. An
  optional **cloud tier** (Claude/Gemini) is a paid fallback and powers the
  manual ✨ identify button. The 24 h iNat token **auto-refreshes** from a
  stored session cookie — no daily re-paste.

  > **Note — the on-device model was abandoned.** Earlier firmware ran a
  > quantized TFLite-Micro classifier ("nordic") on the S3 itself. It was given
  > up and removed (v0.74.0 / FSD §3.2): usable accuracy needed **per-species
  > retraining**, and the only readily-available training images are glossy,
  > subject-filling "hero" shots that **don't transfer** to a fixed, low-res
  > feeder-cam view — features tuned to those photos measured *worse*, not
  > better, on real captures. Removing it also freed ~6 MB of PSRAM and
  > ~110 KB of flash. Classification is now online-only (iNaturalist + optional
  > cloud).
- **Gallery** — browse captures by day; each image carries a per-image
  **classification state** (unclassified / classified / human-confirmed /
  no-bird / confirmed-no-bird), colour-badged on the thumbnail. **Filter** by
  state, including a **Near threshold** view that surfaces under-scored birds
  (top guess just below the confidence setting) for quick review. Delete a
  single photo, multi-select delete, delete a whole day, or **wipe a day**
  (photos + that day's stats together).
- **Human-in-the-loop classification** — accept the model's guess with one
  click (**✓ confirm**), correct it (**✎ relabel**, with autocomplete + a
  free-text/"No bird" option), or re-run **identify** on a saved photo (which
  now persists its result). Confirmed labels become the authoritative tag *and*
  ground-truth training data: a `training-data/export-labels.ps1` tool pulls
  every confirmed sighting into a retrain-ready dataset (see FSD §3.2.1).
- **Stats** — visits per day, species leaderboard, activity-by-hour, a
  confirmed **false-positive** row, per-row last-10 image thumbnails, plus a
  **recheck** (re-run species ID over a day or a selection) and one-click
  **Reset Statistics** for clearing history without touching photos.
- **Settings** — placement mode, motion tuning, confidence threshold, boot
  quarantine, capture count/interval/cool-down, species set (all / Norway-only
  region filter), motion-area zoom (best-of whole-vs-crop), iNaturalist token +
  auto-refresh session cookie, optional cloud provider + keys,
  display language (English/Norwegian), retention cap, stream quality, camera
  **resolution** (VGA–SXGA) and **contrast**, image **rotation** (0/90/180/270°
  to correct how the camera is physically mounted), timezone, NTP server, IR
  LED mode.
- **Debug tab** — heap/uptime/WiFi health, SD card status, camera sensor info,
  species-ID model/label counts.
- **WiFi tab** — network scan + credential save, DHCP or static IP.
- **OTA Update** — upload a `.bin` from the browser; dual OTA partitions with
  automatic bootloader rollback if an image fails to boot cleanly.
- **No cloud, no accounts, no telemetry** — everything above runs entirely on
  the device and your LAN.

## Hardware

| Part | Notes |
|---|---|
| ESP32-S3 camera board | Primary target — reference unit is a generic "ESP32-S3-CAM" (N16R8, OV2640). 8 MB PSRAM required for species ID. |
| AI-Thinker ESP32-CAM | Works, but without species identification (FSD §3.2) |
| microSD card, FAT32 | Capture storage + visit log |
| PIR sensor *(optional)* | First-stage motion trigger |
| IR LED *(optional)* | Inside-nest-box night viewing |

Pin maps live in [`main/board_config.h`](main/board_config.h) — select your
board there (or add a block for a new one).

## Build & flash

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v6.x.

```sh
idf.py set-target esp32s3      # or esp32 for AI-Thinker ESP32-CAM
idf.py -p COMx flash monitor
```

Or skip the toolchain entirely: download a prebuilt `.bin` from
[Releases](../../releases) and flash it (first time via esptool, afterwards
via the web UI's **OTA Update** tab).

## Species identification setup

Classification is **online, via iNaturalist** (no model to install). In
**Settings → iNaturalist**: paste an API token, then paste your
`_inaturalist_session` cookie once so the box **auto-refreshes** the 24 h token
itself. Turn on **Norway only** (region filter) so out-of-region guesses are
rejected. Optionally add a **cloud provider** (Claude/Gemini) key as a paid
fallback. The old on-device model is gone — see the note under *Features* and
[docs/MODEL.md](docs/MODEL.md) (kept for history).

## First startup

1. Power the device. It opens a WiFi access point **`BirdBox-Config`**
   (password `birdbox1234`).
2. Connect to it and open `http://192.168.4.1`.
3. Pick your home WiFi from the scanned list, enter the password, save.
4. The device reboots onto your network — open its IP in a browser.

To reset WiFi credentials: hold the boot button ≥ 5 s while powering on.

## Web UI

**Live** stream (MJPEG, also usable directly at `/stream` in Home
Assistant/VLC), with a quick rotation toggle · **Gallery** of captures with
per-image classification-state badges, state filters (incl. Near-threshold),
one-click confirm / relabel / identify, multi-select delete, and combined
photo+stats day wipe · **Stats** (visits per day, species leaderboard,
activity by hour, false-positive row, recheck, reset button) · **Settings**
(motion, boot quarantine, species ID, camera resolution/contrast/rotation,
storage, system) · **Debug** · **WiFi** (incl. static IP) · **OTA Update**.

Everything is also available as JSON under `/api/…` — see FSD §6.

## Privacy & security posture

No cloud, no accounts, no telemetry. The device is LAN-only by design; for
remote viewing use your own VPN (WireGuard/Tailscale). See FSD §3.3.

## License

[MIT](LICENSE)
