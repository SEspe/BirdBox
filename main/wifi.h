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

/* Re-applies g_settings.ntp_server (FSD §3.4/§5): a no-op until the first
 * successful connect starts SNTP, after which the Settings tab can call
 * this to switch server without a reboot. */
void wifi_restart_sntp(void);

/* Credential save handshake with web_server.c (portal form and WiFi tab):
 * the HTTP handler fills g_new_ssid/g_new_pass, sets g_new_slot (0 = primary,
 * 1 = alternative "alt1"), and sets g_wifi_save_requested; wifi.c persists to
 * NVS and reboots. Saving slot 1 with an empty SSID removes the alternative. */
extern volatile bool g_wifi_save_requested;
extern volatile int  g_new_slot;
extern char          g_new_ssid[64];
extern char          g_new_pass[64];

/* Two stored networks (FSD §4.7): the device tries the primary first, then the
 * alternative, at boot, and fails over between them in service. */
#define WIFI_SLOT_PRIMARY  0
#define WIFI_SLOT_ALT      1

/* Configured SSID for a slot (0/1), "" if none — for the WiFi tab, which shows
 * the configured network names. Passwords are never exposed. */
const char *wifi_configured_ssid(int slot);

/* IP configuration (FSD §4.5, RemoteStart v1.37 design): DHCP by default,
 * optional static IP. Loaded from NVS at boot, read by GET /api/ipconfig,
 * overwritten by POST /api/ipconfig/save before g_ipcfg_save_requested
 * triggers persist + reboot. An invalid saved config falls back to DHCP. */
extern bool          g_ipcfg_static;
extern char          g_ipcfg_ip[16];
extern char          g_ipcfg_mask[16];
extern char          g_ipcfg_gw[16];
extern char          g_ipcfg_dns[16];
extern volatile bool g_ipcfg_save_requested;

/* Diagnostics for the Debug card / /api/sysinfo (FSD §5) */
extern volatile uint32_t g_wifi_disconnect_count;
extern volatile int64_t  g_wifi_last_disc_ts_us;   /* 0 = never since boot */
