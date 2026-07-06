/* BirdBox — WiFi nest box / feeder camera with AI species identification.
 *
 * Spec: FSD_BirdBox.md (repo root). Architecture follows the RemoteStart
 * project's ESP-IDF conventions; each subsystem lives in its own module:
 *
 *   wifi.c        provisioning portal + STA connection      (FSD §4)
 *   web_server.c  embedded UI, REST API, /stream, OTA       (FSD §5, §6, §8)
 *   camera.c      sensor init / frame access                (FSD §2.1)
 *   motion.c      motion detection                          (FSD §3.1)
 *   capture.c     visit-event capture pipeline              (FSD §3.1, §3.4)
 *   classify.c    on-device species identification          (FSD §3.2)
 *   storage.c     microSD captures + visit log              (FSD §7)
 *   settings.c    NVS-backed runtime settings               (FSD §5)
 *
 * First startup: connect to "BirdBox-Config" WiFi (pw: birdbox1234),
 * pick your network from the scan, save — device reboots onto your LAN.
 */
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "version.h"
#include "settings.h"
#include "storage.h"
#include "camera.h"
#include "wifi.h"
#include "web_server.h"
#include "motion.h"
#include "classify.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "%s v%s starting", FIRMWARE_NAME, FIRMWARE_VERSION);

    /* NVS — required by WiFi and settings */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(settings_load());
    ESP_ERROR_CHECK(storage_init());     /* device runs without SD (FSD §7) */
    if (camera_init() != ESP_OK)         /* degrade, don't brick: UI shows "no camera" */
        ESP_LOGE(TAG, "continuing without camera");
    ESP_ERROR_CHECK(wifi_start());       /* portal on first boot (FSD §4) */
    ESP_ERROR_CHECK(web_server_start());
    ESP_ERROR_CHECK(classify_init());
    ESP_ERROR_CHECK(motion_start());

    /* TODO(FSD §3.4): SNTP start + timezone from settings once wifi.c is
     * implemented — events before first sync get placeholder timestamps. */

    ESP_LOGI(TAG, "boot complete");
}
