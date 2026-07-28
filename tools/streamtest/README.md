# streamtest — PSRAM-free board bringup

A standalone ESP-IDF app for answering one question about a candidate
ESP32-S3 CAM board: **is anything wrong with it besides PSRAM?**

BirdBox itself cannot run on a board with dead PSRAM (frame buffers and the
gallery label table live there), and such a board hangs in `cpu_start` long
before it can serve a page — so it tells you nothing about itself. This image is
built with `CONFIG_SPIRAM` **off** and puts the camera frame buffer in internal
DRAM (`CAMERA_FB_IN_DRAM`). That caps resolution to QVGA, but it exercises the
sensor, XCLK/SCCB wiring, WiFi, TCP, the HTTP stack and the power path.

- **Streams** → the board is healthy apart from PSRAM.
- **Doesn't stream** → the fault is wider than PSRAM; read the serial log.

## Endpoints

| Route | What |
|---|---|
| `GET /` | one-page viewer (`<img src=/stream>`) |
| `GET /stream` | multipart MJPEG |
| `GET /health` | chip rev, embedded-PSRAM efuse bit, frames sent, fb failures, free DRAM |

## Setup

```sh
cp main/wifi_creds.h.example main/wifi_creds.h
# edit main/wifi_creds.h — set SSID + password
```

`wifi_creds.h` is gitignored so the password stays out of the repo. Leave
`TEST_WIFI_SSID` empty (`""`) to run as a SoftAP instead — SSID `BirdBox-Test`,
password `birdbox1234`, stream at `http://192.168.4.1/`.

## Build & flash

`export.ps1` is broken on this machine; call `idf.py` through the IDF Python env
(PowerShell — `idf_tools.py` refuses to run under MSys/Bash):

```powershell
$py = "C:\Users\Stein\.espressif\python_env\idf6.0_py3.11_env\Scripts\python.exe"
$env:IDF_PATH = "D:\esp\v6.0.1\esp-idf"
& $py "$env:IDF_PATH\tools\idf_tools.py" export --format key-value | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { $n=$matches[1]; $v=$matches[2].Replace('%PATH%',$env:PATH); Set-Item "env:$n" $v } }
& $py "$env:IDF_PATH\tools\idf.py" set-target esp32s3
& $py "$env:IDF_PATH\tools\idf.py" build
cd build
& $py -m esptool --chip esp32s3 -p COM17 -b 460800 --before default-reset --after hard-reset write-flash "@flash_args"
```

Enumerate the port first — it changes across replugs and between boards:
`[System.IO.Ports.SerialPort]::GetPortNames()`

Then read the serial log for the address:

```
I (nnnn) streamtest: ==================================================
I (nnnn) streamtest:   STREAM READY:  http://192.168.1.xxx/
I (nnnn) streamtest: ==================================================
```

Reading the boot log resets the board via RTS (no non-invasive peek on this
wiring) — that's expected.

## Notes

- Pin map is copied from `main/board_config.h` (`BOARD_ESP32S3_CAM_GENERIC`),
  which stays the source of truth. Re-check if that file changes.
- QVGA is the safe default without PSRAM. VGA usually also fits; SVGA and up
  fail to allocate — `/health` shows the DRAM headroom, and a failed
  `esp_camera_init` logs the largest free block.
- Not part of the BirdBox firmware and not on its version/FSD track.
