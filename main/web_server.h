#pragma once
#include "esp_err.h"

/* Embedded web UI + REST API + MJPEG stream (FSD §5, §6).
 * Tabs: Live | Gallery | Stats | Settings | Debug | WiFi | OTA Update. */

esp_err_t web_server_start(void);

/* Called from wifi.c's SNTP sync callback once a real NTP sync lands, so the
 * clockSrc reported by /api/status reflects it instead of staying latched on
 * "manual" from an earlier browser-time fallback (FSD §3.4). */
void web_server_note_ntp_sync(void);
