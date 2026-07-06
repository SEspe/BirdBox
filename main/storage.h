#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* microSD storage (FSD §7): FAT32 mount, capture files, monthly visit-log
 * CSVs, retention pruning. All writes sequenced through one writer task.
 * The device must run without a card — features degrade to a clear
 * "no SD card" state, never a silent failure. */

esp_err_t storage_init(void);
bool storage_sd_present(void);
