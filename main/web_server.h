#pragma once
#include "esp_err.h"

/* Embedded web UI + REST API + MJPEG stream (FSD §5, §6).
 * Tabs: Live | Gallery | Stats | Settings | Debug | WiFi | OTA Update. */

esp_err_t web_server_start(void);

/* Called from wifi.c's SNTP sync callback once a real NTP sync lands, so the
 * clockSrc reported by /api/status reflects it instead of staying latched on
 * "manual" from an earlier browser-time fallback (FSD §3.4). */
void web_server_note_ntp_sync(void);

/* SoC die temperature in °C, or -1000 when this target has no on-die sensor
 * or the read failed. Lives here because web_server.c owns the one-time
 * install of the sensor handle; ha.c publishes the same value to Home
 * Assistant that the Debug tab shows (FSD §13). */
float web_soc_temp_c(void);
