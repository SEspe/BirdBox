#pragma once
#include "esp_err.h"

/* Embedded web UI + REST API + MJPEG stream (FSD §5, §6).
 * Tabs: Live | Gallery | Stats | Settings | Debug | WiFi | OTA Update. */

esp_err_t web_server_start(void);
