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

/* Human-readable current frame size, e.g. "SVGA 800x600" (Debug card, §5).
 * Reflects g_settings.resolution, which is applied at camera_init (reboot). */
const char *camera_framesize_str(void);

/* Applies 0/180 rotation at the sensor (hmirror+vflip, free — fixes the
 * live stream, SD captures, and classifier input all at once). 90/270 aren't
 * supported by the OV2640 in hardware, so this leaves the sensor unrotated
 * for those angles; classify.cpp and the web UI correct 90/270 themselves. */
esp_err_t camera_set_rotation(rotation_t rot);

/* Sensor PID for the Debug card (FSD §5); 0 when no camera. */
int camera_get_pid(void);
