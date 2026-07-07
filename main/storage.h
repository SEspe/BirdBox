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
 * Writes are serialized by an internal mutex, shared with the retention
 * pruner (FSD §3.1): every storage_save_jpeg() call rechecks SD usage
 * against g_settings.sd_cap_pct and deletes the oldest capture day-folder(s)
 * — never today's — until back under the cap. */

#define STORAGE_MOUNT_POINT "/sd"

esp_err_t storage_init(void);
bool storage_sd_present(void);

/* Total/free bytes of the mounted card; both 0 when no card. */
void storage_get_info(uint64_t *total, uint64_t *free_bytes);

/* Card identification (CID name, e.g. "BC2QT") for the Debug card (FSD §5);
 * out[0] = '\0' when no card. */
void storage_get_card_name(char *out, size_t out_len);

/* Whether the most recent write attempt (capture or visit-log row) succeeded
 * — the closest thing to a "health" signal FAT/SDMMC offers without a SMART
 * equivalent; true (no news is good news) until a write actually fails. */
bool storage_last_write_ok(void);

/* Saves a JPEG under /sd/captures/<date>/<time>.jpg (SNTP-synced clock, or
 * an uptime-based name under /sd/captures/no-date/ before first sync).
 * On success writes the web path (e.g. "/captures/2026-07-06/183501.jpg")
 * into path_out. */
esp_err_t storage_save_jpeg(const uint8_t *data, size_t len,
                            char *path_out, size_t path_out_len);

/* Appends one CSV row to the monthly visit log (/sd/log/visits-YYYY-MM.csv),
 * writing the header line first when the file is new (FSD §3.4). */
esp_err_t storage_append_visit_log(const char *line);
