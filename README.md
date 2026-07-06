# BirdBox

Open-source WiFi nest box / bird feeder camera on an **ESP32-S3**, built with
**ESP-IDF** (no Arduino, no cloud). It detects bird visits, captures photos to
microSD, identifies the species with an on-device AI model, and serves a live
stream, gallery and visit statistics from its own built-in web page — entirely
on your LAN.

**Full specification:** [FSD_BirdBox.md](FSD_BirdBox.md) — the FSD is the
project's requirements document *and* change record; every functional change
gets a changelog entry there.

> **Status: scaffold.** Project structure, build system and CI are in place;
> the firmware modules under `main/` are stubs marked with `TODO(FSD §…)`.

## Hardware

| Part | Notes |
|---|---|
| ESP32-S3 camera board | Primary target — reference unit is a generic "ESP32-S3-CAM" (N16R8, OV2640); XIAO ESP32S3 Sense and Freenove S3 CAM also supported. 8 MB PSRAM required for species ID. |
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

## First startup

1. Power the device. It opens a WiFi access point **`BirdBox-Config`**
   (password `birdbox1234`).
2. Connect to it and open `http://192.168.4.1`.
3. Pick your home WiFi from the scanned list, enter the password, save.
4. The device reboots onto your network — open its IP in a browser.

To reset WiFi credentials: hold the boot button ≥ 5 s while powering on.

## Web UI

**Live** stream (MJPEG, also usable directly at `/stream` in Home
Assistant/VLC) · **Gallery** of captures with species labels you can correct ·
**Stats** (visits per day, species leaderboard, activity by hour) ·
**Settings** · **Debug** · **WiFi** (incl. static IP) · **OTA Update**.

Everything is also available as JSON under `/api/…` — see FSD §6.

## Privacy & security posture

No cloud, no accounts, no telemetry. The device is LAN-only by design; for
remote viewing use your own VPN (WireGuard/Tailscale). See FSD §3.3.

## License

[MIT](LICENSE)
