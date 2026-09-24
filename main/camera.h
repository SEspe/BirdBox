#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_camera.h"   /* camera_fb_t, used by camera_grab/return */
#include "settings.h"

/* Camera sensor init & frame access (FSD §2.1) — wraps the esp32-camera
 * component with the pins from board_config.h. */

esp_err_t camera_init(void);

/* False when no sensor was found at boot — the web UI shows a clear
 * "no camera" state instead of a broken stream. */
bool camera_available(void);

/* Frame access wrappers — ALL frame grabs must go through these, not
 * esp_camera_fb_get/return directly, so the watchdog (FSD §3.5) sees one
 * authoritative health signal and can safely re-init the sensor without a
 * grab racing a deinit. camera_grab() returns NULL when no frame is
 * available (or during a recovery); always pair a non-NULL grab with a
 * camera_return(). */
camera_fb_t *camera_grab(void);
void         camera_return(camera_fb_t *fb);

/* Camera watchdog (FSD §3.5): a background task that detects a stalled sensor
 * (esp_camera_fb_get() returning NULL for >5 s) and auto-recovers it with a
 * deinit + XCLK-drain + re-init cycle. Recovers ESP32-side/DMA/XCLK stalls;
 * the OV2640 analog-core latch needs a real power cycle, which this board
 * can't do (PWDN/RESET unwired) — see FSD §3.5 for the HW mod. Call once after
 * camera_init(). */
esp_err_t camera_watchdog_start(void);

/* Watchdog telemetry for /api/sysinfo (Debug tab). */
uint32_t camera_recovery_count(void);     /* successful re-inits since boot   */
int      camera_last_recovery_ago_s(void);/* seconds since last, -1 if none   */
bool     camera_fault(void);              /* true = auto-recovery gave up,
                                             needs a manual power cycle        */
uint32_t camera_fault_clears(void);       /* times a delivered frame cleared a
                                             fault (v2.97) — a non-zero count
                                             means the flag had gone stale     */

/* Re-programs the sensor's JPEG quality (lower = better) at runtime — the
 * Settings tab applies stream quality without a reboot (FSD §5). */
esp_err_t camera_set_quality(uint8_t quality);

/* OV2640 contrast, clamped to -2..+2, applied live (FSD §5). The sensor has
 * no sharpness control, so contrast is the closest supported "crispness" knob. */
esp_err_t camera_set_contrast(int level);

/* OV2640 auto-exposure level, clamped to -2..+2, applied live (FSD §5): raises
 * (+) or lowers (-) the AE brightness target so a scene the default metering
 * renders too dark/bright can be corrected without fixing the exposure. */
esp_err_t camera_set_ae_level(int level);

/* Fixed-short-exposure ("fast shutter") mode, applied live (FSD §2.1): fixes
 * the sensor's integration time short instead of letting AEC lengthen it in
 * dim light (the main source of motion blur on a close/fast bird), while
 * leaving AGC on to auto-compensate brightness — trades noise for less
 * blur. false restores normal auto exposure. */
esp_err_t camera_set_fast_shutter(bool enable);

/* No frame size is running (camera down / never initialized). */
#define CAMERA_RES_NONE 0xFF

/* Human-readable frame size ACTUALLY running, e.g. "SVGA 800x600" (Debug card,
 * §5) — "none (camera down)" when there isn't one. This is deliberately not
 * g_settings.resolution: that's the *request*, and it differs from reality both
 * after a saved-but-not-rebooted change and after camera_init's degrade ladder
 * steps down. Reporting the request is what hid a degraded boot (v1.96). */
const char *camera_framesize_str(void);

/* The running frame size as a RES-table index (comparable to
 * g_settings.resolution), or CAMERA_RES_NONE. Differs from the setting => the
 * box degraded at boot and a reboot would retry the request. */
uint8_t camera_active_res(void);

/* Applies the sensor's share of the view transform (FSD §5): BOTH mirrors and
 * exactly 180 deg of rotation, all of which the sensor does for free as
 * hmirror/vflip — lossless, and it corrects the SD captures and the classifier
 * input, not just the screen.
 *
 * 180 deg IS both mirrors, so rotation and mirroring compose by XOR: at 180 a
 * requested mirror cancels that axis instead of adding to it. Hence one call
 * taking all three, rather than separate rotation and mirror setters that would
 * fight over the same two registers.
 *
 * No OV sensor can rotate a quarter turn, so 90/270 is the only part left to
 * the browser (live view and image grids); at those angles the JPEG on the card
 * keeps the sensor's orientation. The web UI must skip its own rotation at
 * exactly 180 or it would be applied twice — see applyView() in web_server.c.
 * Since v2.89 the UI does NOT mirror at all; this owns it. */
esp_err_t camera_set_view(uint16_t deg, bool mirror_h, bool mirror_v);

/* Sensor PID for the Debug card (FSD §5); 0 when no camera. */
int camera_get_pid(void);

/* ── Sensor capabilities (FSD §2.1) ─────────────────────────────────────────
 * BirdBox runs on whatever sensor the board carries — an OV2640 (2 MP, the
 * reference unit) or an OV5640 (5 MP), with OV3660 in between. Rather than
 * branch on PID all over the firmware, camera.c probes the sensor once at init
 * and publishes what it can actually do. Everything else (Settings UI, the
 * /api/settings clamp, the Debug card) reads this, so adding a sensor is a
 * driver matter, not a firmware-wide edit.
 *
 * The probes are empirical, not a hardcoded PID table: max_res comes from the
 * driver's own camera_sensor[] max_size, and sharpness/denoise are detected by
 * *calling* the setter and checking for the -1 the OV2640 stubs return. A
 * sensor the driver supports but this file has never heard of therefore still
 * gets the right controls. */
typedef struct {
    int         pid;          /* sensor PID, 0 when no camera                  */
    const char *name;         /* "OV2640"/"OV5640"/… from the driver's table,
                                 "none" when no camera, "unknown" if the PID
                                 isn't in camera_sensor[]                      */
    uint8_t     max_res;      /* highest RES-table index this sensor supports;
                                 CAMERA_RES_NONE when no camera                */
    bool        sharpness;    /* true = a real sharpness control (OV5640-class;
                                 the OV2640's set_sharpness is a -1 stub)      */
    bool        denoise;      /* true = a real denoise control, same story      */
    bool        autofocus;    /* true = AF firmware loaded into the sensor OK.
                                 NOT proof a VCM lens is fitted — plenty of
                                 OV5640 modules are fixed-focus and still take
                                 the firmware. Focus status is the honest tell. */
} camera_caps_t;

/* Never NULL — with no camera it reports pid 0 / "none" / max_res
 * CAMERA_RES_NONE and every capability false. */
const camera_caps_t *camera_caps(void);

/* Number of entries in camera.c's RES table (frame sizes the firmware knows).
 * The Settings dropdown offers indices 0..camera_caps()->max_res of these. */
uint8_t camera_res_count(void);

/* Human-readable label for a RES-table index, e.g. "HD 1280x720"; NULL when
 * the index is out of range. */
const char *camera_res_str(uint8_t idx);

/* OV5640-class sharpness, clamped to -3..+3, applied live (FSD §5). Returns
 * ESP_ERR_NOT_SUPPORTED on a sensor without one (the OV2640), which is not an
 * error — the Settings UI hides the control there. */
esp_err_t camera_set_sharpness(int level);

/* OV5640-class denoise strength 0..8, 0 = off, applied live (FSD §5). Same
 * ESP_ERR_NOT_SUPPORTED contract as camera_set_sharpness. */
esp_err_t camera_set_denoise(int level);

/* Autofocus (OV5640 + a VCM lens, FSD §5). mode is a focus_mode_t: OFF leaves
 * the lens wherever it sits, AUTO runs the sensor's continuous AF loop, MANUAL
 * pins it at `pos` (0..1023, near→far). ESP_ERR_NOT_SUPPORTED when AF never
 * initialized. */
esp_err_t camera_set_focus(uint8_t mode, uint16_t pos);

/* One-shot refocus, for the Settings "Focus now" button — meaningful in any
 * mode, and the only way to refocus in OFF/MANUAL. Blocks up to ~2 s waiting
 * for the sensor to settle. */
esp_err_t camera_focus_now(void);

/* Focus state for the Debug card: true when the sensor last reported a
 * successful lock. Always false without AF. */
bool camera_focus_locked(void);

/* Why autofocus is unavailable, for the Debug card — "" when it IS available.
 * Distinguishes "switched off" from "the sensor refused the AF firmware", which
 * is the answer someone needs when working out whether their OV5640 module
 * actually has a focus motor. */
const char *camera_af_error(void);

/* ── Night sleep (FSD §14) ──────────────────────────────────────────────────
 * Powers the sensor down for the night and brings it back. Distinct from the
 * watchdog's recovery: this one deliberately STAYS down until camera_wake().
 * While asleep camera_grab() returns NULL (so the stream and snapshots show
 * the normal "no camera" state) and the watchdog ignores the sensor entirely
 * rather than trying to recover it.
 * camera_sleep() returns ESP_ERR_INVALID_STATE if a grab is wedged in the
 * driver and cannot be drained — the caller should stay awake rather than
 * deinit under it. */
esp_err_t camera_sleep(void);
esp_err_t camera_wake(void);
bool      camera_asleep(void);

/* Increments on every successful sensor initialisation — boot, watchdog
 * recovery and night-sleep wake alike.
 *
 * ANY module that caches sensor register state MUST compare this against the
 * generation it last configured, and re-apply on a change. camera_hw_init()
 * resets the sensor to a known baseline (notably fast shutter OFF), so a cache
 * that survives an init silently describes a sensor that no longer matches it.
 * That exact bug inverted night detection: motion.c believed fast shutter was
 * engaged after a wake when the hardware had dropped it, so the box metered the
 * night with full auto exposure, read it as "bright", and stayed awake all
 * night — then slept at dawn (FSD §14/v3.08). */
uint32_t camera_init_generation(void);
