#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* WiFi provisioning & connection (FSD §4) — RemoteStart pattern:
 * first-boot SoftAP portal with scan, NVS credentials, indefinite in-service
 * reconnect once ever-connected, WIFI_PS_NONE + max TX power. */

/* Connects as STA using stored credentials. With no stored credentials — or
 * when the boot-time connect fails — opens the BirdBox-Config portal and
 * never returns (the portal saves + reboots). Returns ESP_OK once connected. */
esp_err_t wifi_start(void);

bool wifi_is_connected(void);
bool wifi_in_portal_mode(void);

/* Credential save handshake with web_server.c (portal form and, later, the
 * WiFi tab): the HTTP handler fills g_new_ssid/g_new_pass and sets
 * g_wifi_save_requested; wifi.c persists to NVS and reboots. */
extern volatile bool g_wifi_save_requested;
extern char          g_new_ssid[64];
extern char          g_new_pass[64];

/* Diagnostics for the Debug card / /api/sysinfo (FSD §5) */
extern volatile uint32_t g_wifi_disconnect_count;
extern volatile int64_t  g_wifi_last_disc_ts_us;   /* 0 = never since boot */
