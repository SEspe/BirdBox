#include "storage.h"
#include "board_config.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "storage";

static bool              s_sd_present = false;
static sdmmc_card_t     *s_card = NULL;
static SemaphoreHandle_t s_write_mtx;

esp_err_t storage_init(void)
{
    s_write_mtx = xSemaphoreCreateMutex();

#if !SD_USE_SDMMC
    ESP_LOGE(TAG, "SPI-mode SD not implemented yet (this board uses SDMMC)");
    return ESP_OK;
#else
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;                       /* 1-bit bus: only D0 is routed on these boards */
    slot.clk   = SD_PIN_CLK;
    slot.cmd   = SD_PIN_CMD;
    slot.d0    = SD_PIN_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,   /* never format a user's card uninvited */
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(STORAGE_MOUNT_POINT, &host, &slot,
                                            &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed (%s) — running without SD. "
                 "Card must be FAT32; check SD pins in board_config.h",
                 esp_err_to_name(err));
        return ESP_OK;   /* degrade, don't abort (FSD §7) */
    }

    s_sd_present = true;
    ESP_LOGI(TAG, "SD mounted: %s, %llu MB",
             s_card->cid.name,
             ((uint64_t) s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024));

    mkdir(STORAGE_MOUNT_POINT "/captures", 0775);
    mkdir(STORAGE_MOUNT_POINT "/log",      0775);
    mkdir(STORAGE_MOUNT_POINT "/model",    0775);
    return ESP_OK;
#endif
}

bool storage_sd_present(void) { return s_sd_present; }

void storage_get_info(uint64_t *total, uint64_t *free_bytes)
{
    *total = 0;
    *free_bytes = 0;
    if (!s_sd_present) return;
    esp_vfs_fat_info(STORAGE_MOUNT_POINT, total, free_bytes);
}

esp_err_t storage_save_jpeg(const uint8_t *data, size_t len,
                            char *path_out, size_t path_out_len)
{
    if (!s_sd_present) return ESP_ERR_INVALID_STATE;

    /* Build /sd/captures/<day>/<name>.jpg. Before the first SNTP sync the
     * clock reads ~1970 (FSD §3.4) — use an uptime-based name under no-date/
     * instead of a bogus timestamp. */
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char day[32], name[40];
    if (tm_now.tm_year + 1900 < 2020) {
        strlcpy(day, "no-date", sizeof(day));
        snprintf(name, sizeof(name), "up%llu", (unsigned long long) (esp_timer_get_time() / 1000));
    } else {
        strftime(day,  sizeof(day),  "%Y-%m-%d", &tm_now);
        strftime(name, sizeof(name), "%H%M%S",   &tm_now);
    }

    char dir[64], path[128];
    snprintf(dir,  sizeof(dir),  STORAGE_MOUNT_POINT "/captures/%s", day);
    snprintf(path, sizeof(path), "%s/%s.jpg", dir, name);
    /* Same second twice (manual snapshots) — add a suffix rather than overwrite */
    struct stat st;
    for (char c = 'b'; stat(path, &st) == 0 && c <= 'z'; c++)
        snprintf(path, sizeof(path), "%s/%s%c.jpg", dir, name, c);

    xSemaphoreTake(s_write_mtx, portMAX_DELAY);
    mkdir(dir, 0775);
    FILE *f = fopen(path, "wb");
    size_t written = 0;
    if (f) {
        written = fwrite(data, 1, len, f);
        fclose(f);
    }
    xSemaphoreGive(s_write_mtx);

    if (!f || written != len) {
        ESP_LOGE(TAG, "write failed: %s (%u/%u bytes)", path,
                 (unsigned) written, (unsigned) len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "saved %s (%u bytes)", path, (unsigned) len);
    if (path_out)
        strlcpy(path_out, path + strlen(STORAGE_MOUNT_POINT), path_out_len);
    return ESP_OK;
}
