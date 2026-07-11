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

/* Deletes every /sd/log/visits-*.csv file — clears historic species
 * recognition / stats (FSD §3.4) without touching saved photos on SD.
 * Returns the number of files deleted (0 if none/no SD). */
int storage_reset_stats(void);

/* Removes just one capture day's visit-log rows (FSD §3.4): for a real
 * "YYYY-MM-DD" date it rewrites that month's CSV keeping the header + every
 * row that isn't that day; for "no-date" it deletes the unsynced log. Used by
 * the Gallery "wipe day" action. Returns rows removed (0 if none/no SD). */
int storage_reset_stats_day(const char *date);

/* Sets the user-confirmed species on the visit row whose first_frame basename
 * is `file` (FSD §3.4/v1.51): writes `common` to the "corrected" column and
 * `latin` to the "latin" column (both CSV-sanitized), keeping the model's
 * original guess in "species" for reference. If no row matches the image, a
 * new user-confirmed row is appended (timestamp parsed from the filename).
 * A non-empty "corrected" column is the human-confirmed flag other code and
 * future training export key on. */
esp_err_t storage_relabel(const char *date, const char *file,
                          const char *common, const char *latin);

/* Accept the model's current classification as human-confirmed (§3.4/v1.59):
 * copies the row's species into its "corrected" column. Returns ESP_ERR_NOT_FOUND
 * when there's nothing confirmable (no row, a sentinel/no-latin row, or already
 * confirmed). Never adds a row. */
esp_err_t storage_confirm(const char *date, const char *file);

/* The single-writer lock (FSD §7) for callers doing their own SD writes
 * (model upload) — capture/log writes take it internally. */
void storage_write_lock(void);
void storage_write_unlock(void);
