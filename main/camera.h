#pragma once
#include "esp_err.h"

/* Camera sensor init & frame access (FSD §2.1) — wraps the esp32-camera
 * component with the pins from board_config.h. */

esp_err_t camera_init(void);
