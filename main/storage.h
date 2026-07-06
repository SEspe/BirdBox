#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* microSD storage (FSD §7): FAT32 mount at /sd, capture files under
 * /sd/captures/YYYY-MM-DD/, monthly visit-log CSVs under /sd/log/.
 * The device runs without a card — features degrade to a clear
 * "no SD card" state, never a silent failure.
 *
 * Writes are serialized by an internal mutex; the queue-based writer task
 * (FSD §7) arrives with the motion-capture pipeline, which is the first
 * producer that needs asynchronous writes. */

#define STORAGE_MOUNT_POINT "/sd"

esp_err_t storage_init(void);
bool storage_sd_present(void);

/* Total/free bytes of the mounted card; both 0 when no card. */
void storage_get_info(uint64_t *total, uint64_t *free_bytes);

/* Saves a JPEG under /sd/captures/<date>/<time>.jpg (SNTP-synced clock, or
 * an uptime-based name under /sd/captures/no-date/ before first sync).
 * On success writes the web path (e.g. "/captures/2026-07-06/183501.jpg")
 * into path_out. */
esp_err_t storage_save_jpeg(const uint8_t *data, size_t len,
                            char *path_out, size_t path_out_len);
