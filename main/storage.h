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

/* SD write-error auto-recovery counters (FSD v2.14): how many times a failed
 * write was cleared by an automatic unmount+remount, and seconds since the last
 * one (-1 = never). A climbing count is an early sign the card is wearing out. */
uint32_t storage_remount_count(void);
int      storage_last_remount_ago_s(void);

/* Saves a JPEG under /sd/captures/<date>/<time>.jpg (SNTP-synced clock, or
 * an uptime-based name under /sd/captures/no-date/ before first sync).
 * On success writes the web path (e.g. "/captures/2026-07-06/183501.jpg")
 * into path_out. */
esp_err_t storage_save_jpeg(const uint8_t *data, size_t len,
                            char *path_out, size_t path_out_len);

/* Builds the visit-log path for one day: /sd/log/visits-<date>.csv, where
 * `date` is "YYYY-MM-DD" (first 10 chars used) or "no-date" (FSD v2.07 per-day
 * files). The shared source of truth for every per-date reader/writer. */
void storage_visit_log_path(const char *date, char *buf, size_t sz);

/* Appends one CSV row to that day's visit log (/sd/log/visits-YYYY-MM-DD.csv),
 * writing the header line first when the file is new (FSD §3.4/v2.07). */
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

/* Batch form of storage_relabel (§3.4/v1.98): applies ONE (common, latin) label
 * to every image in `files` (an array of `nfiles` basenames) in a SINGLE rewrite
 * of the month's CSV, instead of one full rewrite per image. Rows whose image
 * has no existing entry are appended, exactly as storage_relabel does. `*applied`
 * (may be NULL) receives how many rows were written. Same write lock + CSV
 * sanitizing as the single-image path. */
esp_err_t storage_relabel_batch(const char *date, const char *const *files,
                                int nfiles, const char *common, const char *latin,
                                int *applied);

/* Backfill the motion ROI on one visit row (§3.4/v1.99): set the "roi" column
 * (field 7, "x0-y0-x1-y1" fractional) on the row whose first_frame basename is
 * `file`, keeping every other column. For the click-to-place ROI backfill of
 * older captures that predate always-on ROI logging. Returns ESP_ERR_NOT_FOUND
 * when no row matches (never adds one — there's nothing to backfill without a
 * row). `roi` must already be a validated "x0-y0-x1-y1" string. */
esp_err_t storage_set_roi(const char *date, const char *file, const char *roi);

/* Append one saved frame's motion box to the per-day frame-ROI sidecar
 * (/log/frameroi-DATE.csv, "file,roi" per line), §3.4. The capture burst
 * re-detects the box on every frame but only the event row records one; this
 * keeps every frame's box so a later human confirm of a follow-up frame can
 * reuse it (storage_relabel fills it in), no manual backfill. `roi` is a
 * "x0-y0-x1-y1" string; empty/`,`-bearing inputs are rejected. */
esp_err_t storage_log_frame_roi(const char *date, const char *file, const char *roi);

/* Accept the model's current classification as human-confirmed (§3.4/v1.59):
 * copies the row's species into its "corrected" column. Returns ESP_ERR_NOT_FOUND
 * when there's nothing confirmable (no row, a sentinel/no-latin row, or already
 * confirmed). Never adds a row. */
esp_err_t storage_confirm(const char *date, const char *file);

/* The single-writer lock (FSD §7) for callers doing their own SD writes
 * (model upload) — capture/log writes take it internally. */
void storage_write_lock(void);
void storage_write_unlock(void);
