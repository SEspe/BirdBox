#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* WiFi provisioning & connection (FSD §4) — RemoteStart pattern:
 * first-boot SoftAP portal with scan, NVS credentials, indefinite in-service
 * reconnect once ever-connected, WIFI_PS_NONE + max TX power. */

/* Connects as STA using stored credentials, or opens the BirdBox-Config
 * portal when none are stored. Blocks until connected or portal mode. */
esp_err_t wifi_start(void);

bool wifi_is_connected(void);
bool wifi_in_portal_mode(void);
