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

/* Change counters for the visit logs, so a reader can cache what it parsed
 * (the Stats aggregate, v3.44). `gen` moves on every REWRITE or DELETION of a
 * visit-log file: relabel, confirm, recheck, day/stats reset, the per-day
 * migration, a remount. `appends` moves on every appended row. Appends only
 * ever go to the file for the current date, so a cache of PAST days need only
 * watch `gen`. Any new code that rewrites a visit log must call
 * storage_visit_log_bump(), or the Stats tab will keep showing old numbers. */
uint32_t storage_visit_log_gen(void);
uint32_t storage_visit_log_appends(void);
void     storage_visit_log_bump(void);

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

/* Append one saved frame's motion box to the per-day frame-ROI sidecar
 * (/log/frameroi-DATE.csv, "file,roi" per line), §3.4. The capture burst
 * re-detects the box on every frame but only the event row records one; this
 * keeps every frame's box so a later human confirm of a follow-up frame can
 * reuse it (storage_relabel fills it in), no manual backfill. `roi` is a
 * "x0-y0-x1-y1" string; empty/`,`-bearing inputs are rejected. */
esp_err_t storage_log_frame_roi(const char *date, const char *file, const char *roi);

/* Read side of that sidecar, for callers that need EVERY frame's box rather than
 * just the event row's (recheck, v2.47). Load the day once (caller frees; NULL if
 * absent), then look each frame up in the buffer — loading per frame would re-read
 * the file thousands of times in a day-wide recheck. `out` gets "x0-y0-x1-y1". */
char *storage_frameroi_load(const char *date);
bool  storage_frameroi_find(const char *buf, const char *file, char *out, size_t outsz);

/* Accept the model's current classification as human-confirmed (§3.4/v1.59):
 * copies the row's species into its "corrected" column. Returns ESP_ERR_NOT_FOUND
 * when there's nothing confirmable (no row, a sentinel/no-latin row, or already
 * confirmed). Never adds a row. */
esp_err_t storage_confirm(const char *date, const char *file);

/* The single-writer lock (FSD §7) for callers doing their own SD writes
 * (model upload) — capture/log writes take it internally. */
void storage_write_lock(void);
void storage_write_unlock(void);

/* ── Capture file layout (FSD §3.1) ──────────────────────────────────────────
 * Two paths exist for every capture and only these helpers know the difference.
 *
 *   LOGICAL   /captures/<day>/<name>.jpg
 *     What the visit log stores, what every URL and API parameter carries, and
 *     what the retrain export downloads. It has never changed and must not.
 *
 *   PHYSICAL  /captures/<day>/<HH>/<name>.jpg          (since v3.41)
 *     Where the bytes actually are. FATFS resolves a filename by scanning its
 *     directory linearly, so every open costs time proportional to the number of
 *     files beside it — a flat day-folder reached 2647 entries on an ordinary
 *     day and the walk measured 0.155 ms per entry, i.e. ~0.4 s per open by
 *     evening, paid inside the detect task on every frame of every burst.
 *     Bucketing by hour bounds a folder at one hour's captures and makes the
 *     cost flat through the day instead of ramping.
 *
 * The bucket is derived from the name ("YYYY-MM-DD_HH-..."), so the mapping
 * needs no stored state and no migration: captures written before v3.41 are flat
 * and every lookup here falls back to the flat location, so old days, old visit
 * log rows and old bookmarks keep working untouched. Pre-SNTP "no-date" captures
 * carry no hour and stay flat by construction. */

/* Build the physical path for `day`/`name`. `bucket` selects the hour folder
 * (ignored when the name carries no hour). */
void storage_capture_fs_path(const char *day, const char *name, bool bucket,
                             char *out, size_t out_len);

/* Split a logical capture path ("/captures/<day>/<name>", mount prefix optional)
 * into its day folder and bare filename. */
bool storage_capture_split(const char *logical, char *day, size_t dsz,
                           char *name, size_t nsz);

/* fopen() a capture: hour bucket first, flat second. Use for reads. */
FILE *storage_capture_fopen(const char *day, const char *name, const char *mode);

/* Same, from a logical path ("/captures/<day>/<name>.jpg", with or without the
 * mount prefix). Returns NULL if the path is not a capture path. */
FILE *storage_capture_fopen_logical(const char *logical, const char *mode);

/* Physical path of an existing capture (bucket if present, else flat), for
 * callers that must pass a path to another module. */
void storage_capture_resolve(const char *day, const char *name,
                             char *out, size_t out_len);

/* unlink() a capture in either layout. Returns 0 on success. */
int storage_capture_unlink(const char *day, const char *name);

/* Visit every capture of `day` across BOTH layouts, newest-agnostic order (the
 * caller sorts). `cb` gets the bare name. Returns how many were visited. */
int storage_capture_foreach(const char *day,
                            void (*cb)(const char *name, void *ctx), void *ctx);

/* Remove a whole day: every bucket, every file, then the folders. */
void storage_capture_remove_day(const char *day);

/* ── SD self-test (FSD §3.1) ─────────────────────────────────────────────────
 * Manual, operator-triggered throughput + integrity check. It exists because the
 * numbers already on the Debug tab CANNOT answer "is the card healthy?":
 * `sdReadMs` times path resolution as well as the read, so it rises with the
 * number of files in a day folder and says nothing about the card (v3.40 — that
 * confusion produced a wrong "replace the card" verdict). This writes a known
 * pattern to a fixed-size temp file, reads it back, verifies it byte for byte
 * and reports MB/s for each direction — a figure independent of directory size.
 *
 * Not periodic, by decision: the failure actually seen in the field was binary
 * (the card vanished), and a binary fault wants an alarm, not a benchmark. Each
 * run appends one row to /log/sdhealth.csv so repeated manual runs still build
 * the trend that makes a single number interpretable. */
typedef struct {
    bool     ok;              /* test completed and verified                   */
    uint32_t bytes;           /* size written/read                             */
    uint32_t write_ms;        /* ms to write it                                */
    uint32_t read_ms;         /* ms to read it back                            */
    uint32_t write_kbs;       /* kB/s write                                    */
    uint32_t read_kbs;        /* kB/s read                                     */
    bool     verify_ok;       /* read-back matched the pattern byte for byte   */
    uint32_t bad_offset;      /* first mismatching byte, when verify failed    */
    char     err[48];         /* human-readable reason when ok == false        */
} sd_test_t;

/* Runs the test. Refuses (ESP_ERR_INVALID_STATE) when no card is mounted; the
 * CALLER is responsible for refusing while a capture or classification is in
 * flight, since this holds the single-writer lock for its duration. */
esp_err_t storage_sd_selftest(sd_test_t *out);

/* The last result this boot, or NULL if the test has not been run. */
const sd_test_t *storage_sd_selftest_last(void);
