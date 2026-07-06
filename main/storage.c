#include "storage.h"
#include "board_config.h"
#include "esp_log.h"

static const char *TAG = "storage";

static bool s_sd_present = false;

esp_err_t storage_init(void)
{
    /* TODO(FSD §7): mount FAT32 via SDMMC or SPI per board_config.h
     * (SD_USE_SDMMC / SD_PIN_CS), create /captures /log /model dirs,
     * start the single writer task, expose free/size for /api/status. */
    ESP_LOGW(TAG, "storage_init: not implemented (scaffold) — running without SD");
    return ESP_OK;
}

bool storage_sd_present(void) { return s_sd_present; }
