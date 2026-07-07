#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "settings.h"

/* Camera sensor init & frame access (FSD §2.1) — wraps the esp32-camera
 * component with the pins from board_config.h. */

esp_err_t camera_init(void);

/* False when no sensor was found at boot — the web UI shows a clear
 * "no camera" state instead of a broken stream. */
bool camera_available(void);

/* Re-programs the sensor's JPEG quality (lower = better) at runtime — the
 * Settings tab applies stream quality without a reboot (FSD §5). */
esp_err_t camera_set_quality(uint8_t quality);

/* OV2640 contrast, clamped to -2..+2, applied live (FSD §5). The sensor has
 * no sharpness control, so contrast is the closest supported "crispness" knob. */
esp_err_t camera_set_contrast(int level);

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
