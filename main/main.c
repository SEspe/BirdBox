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
#include <stdlib.h>
#include <time.h>

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"   /* heap guard: largest-free-block watch (v2.50) */
#include "esp_system.h"      /* esp_restart */
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
#include "illum.h"

static const char *TAG = "main";

/* Heap low-water mark + when it was hit, for the Debug card / /api/sysinfo
 * (FSD §5 — RemoteStart's leak-spotting convention). Not static: read
 * directly by web_server.c. */
uint32_t g_heap_min       = UINT32_MAX;
int64_t  g_heap_min_ts_us = 0;

/* Heap guard (FSD v2.50). ESP-IDF's certificate-bundle CA callback
 * (esp_crt_copy_asn1 → esp_crt_ca_cb_callback) leaks ~7 small INTERNAL-DRAM
 * blocks per TLS handshake — confirmed by heap trace, and outside our code. The
 * bytes are negligible but the holes shatter free space, and once the largest
 * contiguous block falls under ~29 KB, every HTTPS handshake fails: iNaturalist
 * classification stops dead while capture carries on, so events pile up
 * unclassified and only a reboot fixes it. Measured ~6 h on a normal day.
 *
 * Silently losing classification is the one outcome this project won't accept
 * (§3.2 never-drop-work), so a reboot is preferable to a box that looks alive
 * and identifies nothing. Reboot when the largest internal block sits below the
 * threshold on THREE consecutive checks (~30 s — never on a transient dip during
 * a handshake) and nothing is in flight: no motion, no recheck, no OTA. The
 * threshold sits well above the ~29 KB failure point so we act while TLS still
 * works, and the reboot costs ~20 s of a quiet moment. */
/* 40000 was badly wrong and caused a reboot LOOP (v2.52): the box sits happily
 * at 31744 for hours — that is a normal plateau, not a warning — and TLS only
 * fails at ~28672. A few handshakes step the largest block down past 40 KB
 * within a minute of boot, so the guard fired immediately, and since every
 * reboot restarts the 60 s detection quarantine the box stopped capturing
 * entirely while classifying nothing. Trip just above the real failure point,
 * and never within MIN_UPTIME of a boot so a guard reboot can't chain. */
#define HEAP_GUARD_MIN_BLOCK   30000
#define HEAP_GUARD_STRIKES     3
#define HEAP_GUARD_MIN_UPTIME_S 1800

static void housekeeping_task(void *arg)
{
    g_heap_min       = esp_get_free_heap_size();
    g_heap_min_ts_us = esp_timer_get_time();
    int strikes = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        uint32_t cur = esp_get_free_heap_size();
        if (cur < g_heap_min) { g_heap_min = cur; g_heap_min_ts_us = esp_timer_get_time(); }

        /* Never inside the settle window — a guard reboot must not be able to
         * trigger the next one, and a fresh boot has a backlog to classify. */
        if (esp_timer_get_time() < (int64_t) HEAP_GUARD_MIN_UPTIME_S * 1000000) continue;

        size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (big >= HEAP_GUARD_MIN_BLOCK) { strikes = 0; continue; }
        strikes++;
        ESP_LOGW(TAG, "heap guard: largest internal block %u B (strike %d/%d)",
                 (unsigned) big, strikes, HEAP_GUARD_STRIKES);
        if (strikes < HEAP_GUARD_STRIKES) continue;
        /* An OTA cannot be checked (no busy flag) but is safe to interrupt —
         * dual partitions plus rollback mean a killed upload just fails. */
        bool rc_busy = false;
        classify_recheck_status(&rc_busy, NULL, NULL, NULL, 0);
        if (motion_active() || rc_busy) {
            ESP_LOGW(TAG, "heap guard: work in flight — deferring reboot");
            continue;
        }
        ESP_LOGE(TAG, "heap guard: internal DRAM fragmented (largest block %u B) — "
                      "HTTPS would fail; rebooting to restore classification", (unsigned) big);
        vTaskDelay(pdMS_TO_TICKS(500));      /* let the log drain */
        esp_restart();
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
    /* Apply the timezone at boot, before anything uses localtime (capture
     * folders/filenames, stats) — not just on WiFi connect (FSD §3.4). */
    setenv("TZ", g_settings.timezone, 1);
    tzset();
    ESP_ERROR_CHECK(storage_init());     /* device runs without SD (FSD §7) */
    /* Classifier before camera: the model + arena (~5.5 MB PSRAM) reserve
     * first so a high camera resolution can't starve species ID — camera_init
     * then steps its resolution down to whatever PSRAM is left (FSD §5). */
    ESP_ERROR_CHECK(classify_init());
    if (camera_init() != ESP_OK)         /* degrade, don't brick: UI shows "no camera" */
        ESP_LOGE(TAG, "continuing without camera");
    /* Watchdog right after the camera and BEFORE motion (FSD §3.5). If the
     * camera streams corrupt frames, esp_camera_fb_get() spins CPU-bound; the
     * motion task (prio 4) doing that would starve this prio-1 app_main the
     * instant it's created — so everything that must run at boot (watchdog,
     * web, rollback-validate) is started here first, and motion_start() goes
     * last. The watchdog runs above motion's priority, so it stays schedulable
     * to recover even while a grab spins. */
    ESP_ERROR_CHECK(camera_watchdog_start());
    illum_init();                        /* onboard illuminator, optional hardware */
    ESP_ERROR_CHECK(wifi_start());       /* portal on first boot (FSD §4) */
    ESP_ERROR_CHECK(web_server_start());
    xTaskCreate(housekeeping_task, "housekeep", 2048, NULL, 2, NULL);

    /* Confirms this image booted OK, canceling any pending bootloader
     * rollback (FSD §8). Done before motion_start so a camera that streams
     * garbage (which starves app_main here) can't cause a false rollback of an
     * otherwise-good image. */
    esp_ota_mark_app_valid_cancel_rollback();

    ESP_LOGI(TAG, "boot complete");

    ESP_ERROR_CHECK(motion_start());     /* last: its frame-grab loop is what
                                            spins on a broken camera (FSD §3.5)*/
}
