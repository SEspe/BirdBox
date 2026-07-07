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
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "version.h"
#include "settings.h"
#include "storage.h"
#include "camera.h"
#include "wifi.h"
#include "web_server.h"
#include "motion.h"
#include "classify.h"

static const char *TAG = "main";

/* Heap low-water mark + when it was hit, for the Debug card / /api/sysinfo
 * (FSD §5 — RemoteStart's leak-spotting convention). Not static: read
 * directly by web_server.c. */
uint32_t g_heap_min       = UINT32_MAX;
int64_t  g_heap_min_ts_us = 0;

static void housekeeping_task(void *arg)
{
    g_heap_min       = esp_get_free_heap_size();
    g_heap_min_ts_us = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        uint32_t cur = esp_get_free_heap_size();
        if (cur < g_heap_min) { g_heap_min = cur; g_heap_min_ts_us = esp_timer_get_time(); }
    }
}

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
    /* Classifier before camera: the model + arena (~5.5 MB PSRAM) reserve
     * first so a high camera resolution can't starve species ID — camera_init
     * then steps its resolution down to whatever PSRAM is left (FSD §5). */
    ESP_ERROR_CHECK(classify_init());
    if (camera_init() != ESP_OK)         /* degrade, don't brick: UI shows "no camera" */
        ESP_LOGE(TAG, "continuing without camera");
    ESP_ERROR_CHECK(wifi_start());       /* portal on first boot (FSD §4) */
    ESP_ERROR_CHECK(web_server_start());
    ESP_ERROR_CHECK(motion_start());
    xTaskCreate(housekeeping_task, "housekeep", 2048, NULL, 2, NULL);

    /* Confirms this image booted OK, canceling any pending bootloader
     * rollback (FSD §8) — reaching here means every subsystem above
     * initialized without an ESP_ERROR_CHECK abort. */
    esp_ota_mark_app_valid_cancel_rollback();

    ESP_LOGI(TAG, "boot complete");
}
