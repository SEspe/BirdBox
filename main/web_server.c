#include "web_server.h"
#include "esp_log.h"
#include "esp_http_server.h"

static const char *TAG = "web";

esp_err_t web_server_start(void)
{
    /* TODO(FSD §5, §6): single-file embedded UI + JSON API.
     * Endpoints: /api/status /api/sysinfo /api/events /api/stats/...
     * /api/capture /api/settings /api/ipconfig /api/reboot /stream
     * /captures/... /ota/upload.
     * httpd config (RemoteStart lessons): lru_purge_enable = true,
     * max_uri_handlers with headroom above route count, ESP_LOGE on any
     * failed handler registration. /stream: max 2 clients, else 503.
     * OTA upload: bounded httpd_req_recv() retries (5), dual-partition
     * with rollback. */
    ESP_LOGW(TAG, "web_server_start: not implemented (scaffold)");
    return ESP_OK;
}
