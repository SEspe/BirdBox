#include "wifi.h"
#include "version.h"
#include "esp_log.h"

static const char *TAG = "wifi";

esp_err_t wifi_start(void)
{
    /* TODO(FSD §4): port RemoteStart provisioning —
     *  - NVS ssid/pass in NVS_NAMESPACE; no creds -> SoftAP portal
     *    (WIFI_AP_SSID / WIFI_AP_PASSWORD) with network scan, URL-decoded save
     *  - boot connect: 15 s / 5 retries; failure with stored creds -> APSTA
     *    portal while retrying in background (FSD §4.4)
     *  - once ever-connected: retry indefinitely, never reopen portal
     *  - WIFI_PS_NONE + esp_wifi_set_max_tx_power(84) after esp_wifi_start()
     *  - static IP support via saved ipmode/ip/mask/gw/dns (FSD §4.5) */
    ESP_LOGW(TAG, "wifi_start: not implemented (scaffold)");
    return ESP_OK;
}

bool wifi_is_connected(void)   { return false; }
bool wifi_in_portal_mode(void) { return false; }
