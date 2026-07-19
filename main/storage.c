#include "storage.h"
#include "board_config.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "storage";

static bool              s_sd_present = false;
static sdmmc_card_t     *s_card = NULL;
static SemaphoreHandle_t s_write_mtx;
static bool              s_last_write_ok = true;

/* SD write-error auto-recovery (FSD v2.14): a transient FATFS/driver write error
 * wedges the card into a write-failing state that survives until a remount —
 * seen in the field silently killing captures for hours. On a failed write the
 * writers remount the card and retry once; these count/timestamp the recoveries
 * so the Debug tab can surface a card that's wearing out. */
static uint32_t          s_remount_count = 0;       /* successful recoveries */
static int64_t           s_last_remount_us = 0;     /* last ATTEMPT (cooldown gate) */
static int64_t           s_last_remount_ok_us = 0;  /* last SUCCESS */
#define SD_RECOVER_COOLDOWN_US  (20 * 1000000LL)    /* ≤1 remount attempt / 20 s */

static void storage_migrate_perday(void);   /* one-time monthly→per-day split */

#if SD_USE_SDMMC
/* Mount the card (shared by storage_init and sd_recover). Sets s_sd_present. */
static esp_err_t sd_mount(void)
{
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
    s_sd_present = (err == ESP_OK);
    return err;
}

/* Clear a transient SD write error by unmount+remount. Cooldown-limited so a
 * genuinely dead / read-only card can't thrash the driver on every failed
 * capture. Caller MUST hold s_write_mtx. Returns true only when the card is
 * writable again. A concurrent SD *read* (gallery) can glitch during the
 * unmount — acceptable, and far better than every capture failing until a human
 * notices and reboots. */
static bool sd_recover(void)
{
    int64_t now = esp_timer_get_time();
    if (s_last_remount_us && now - s_last_remount_us < SD_RECOVER_COOLDOWN_US)
        return false;                     /* remounted recently — don't thrash */
    s_last_remount_us = now;

    ESP_LOGW(TAG, "SD write failed — remounting to recover");
    if (s_card) { esp_vfs_fat_sdcard_unmount(STORAGE_MOUNT_POINT, s_card); s_card = NULL; }
    s_sd_present = false;

    esp_err_t err = sd_mount();
    if (err == ESP_OK) {
        s_remount_count++;
        s_last_remount_ok_us = now;
        ESP_LOGW(TAG, "SD remounted OK (recovery #%u)", (unsigned) s_remount_count);
        return true;
    }
    ESP_LOGE(TAG, "SD remount failed (%s) — card may be failing/read-only",
             esp_err_to_name(err));
    return false;
}
#endif

esp_err_t storage_init(void)
{
    s_write_mtx = xSemaphoreCreateMutex();

#if !SD_USE_SDMMC
    ESP_LOGE(TAG, "SPI-mode SD not implemented yet (this board uses SDMMC)");
    return ESP_OK;
#else
    esp_err_t err = sd_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed (%s) — running without SD. "
                 "Card must be FAT32; check SD pins in board_config.h",
                 esp_err_to_name(err));
        return ESP_OK;   /* degrade, don't abort (FSD §7) */
    }
    /* s_sd_present set inside sd_mount() */
    ESP_LOGI(TAG, "SD mounted: %s, %llu MB",
             s_card->cid.name,
             ((uint64_t) s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024));

    mkdir(STORAGE_MOUNT_POINT "/captures", 0775);
    mkdir(STORAGE_MOUNT_POINT "/log",      0775);
    mkdir(STORAGE_MOUNT_POINT "/model",    0775);
    storage_migrate_perday();   /* split any legacy monthly logs into per-day */
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

void storage_get_card_name(char *out, size_t out_len)
{
    if (!s_sd_present) { out[0] = '\0'; return; }
    strlcpy(out, s_card->cid.name, out_len);
}

bool storage_last_write_ok(void) { return s_last_write_ok; }

uint32_t storage_remount_count(void) { return s_remount_count; }
int storage_last_remount_ago_s(void)
{
    return s_last_remount_ok_us
         ? (int) ((esp_timer_get_time() - s_last_remount_ok_us) / 1000000) : -1;
}

void storage_write_lock(void)   { xSemaphoreTake(s_write_mtx, portMAX_DELAY); }
void storage_write_unlock(void) { xSemaphoreGive(s_write_mtx); }

/* ── Retention pruning (FSD §3.1, §7) ────────────────────────────────────
 * Deletes the oldest capture day-folder whole, one at a time, until SD
 * usage drops back under the configured cap (default 80%) or nothing
 * prunable is left. Day-folder names ("YYYY-MM-DD") sort lexicographically
 * by date, so "oldest" is just the smallest name — except "no-date"
 * (pre-SNTP-sync captures), which sorts after every real date and is
 * therefore only ever chosen once no dated folder remains: we can't tell
 * those files' true age, so they're the last resort rather than the first.
 * Today's own folder is always excluded, even if it's the only one left —
 * pruning the day currently being written to would race the capture that's
 * still filling it. Favorites-exemption (FSD §3.1) arrives with §3.4's
 * favorite-flagging; no event carries a favorite flag yet, so there is
 * nothing to exempt. */
static void remove_day_folder(const char *day)
{
    char dir[64];
    snprintf(dir, sizeof(dir), STORAGE_MOUNT_POINT "/captures/%s", day);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char file[128];
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_REG) continue;
        snprintf(file, sizeof(file), "%s/%.40s", dir, e->d_name);
        unlink(file);
    }
    closedir(d);
    rmdir(dir);
}

static void prune_if_over_cap(void)
{
    uint64_t total, free_bytes;
    esp_vfs_fat_info(STORAGE_MOUNT_POINT, &total, &free_bytes);
    if (total == 0) return;

    /* Before SNTP sync the clock reads ~1970 (FSD §3.4), so "today" would
     * compute as a bogus pre-2020 date matching no real day-folder on disk —
     * the "never prune today" guard below would then fail to recognize a
     * real, currently-being-written folder as today's, making it look like
     * fair game as the oldest. Skip pruning entirely until synced rather
     * than risk deleting the folder a just-rebooted device is actively
     * writing into (caught live: a reboot + motion before sync completed
     * wiped that day's folder in testing). New captures already fall back
     * to /captures/no-date/ during this same window (storage_save_jpeg). */
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year + 1900 < 2020) return;

    char today[11];
    strftime(today, sizeof(today), "%Y-%m-%d", &tm_now);

    xSemaphoreTake(s_write_mtx, portMAX_DELAY);
    for (int guard = 0; guard < 64; guard++) {   /* bounded: never spin forever */
        unsigned used_pct = (unsigned) (100 - (free_bytes * 100 / total));
        if (used_pct < g_settings.sd_cap_pct) break;

        DIR *d = opendir(STORAGE_MOUNT_POINT "/captures");
        if (!d) break;
        char oldest[16] = "";
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_type != DT_DIR) continue;
            if (e->d_name[0] == '.') continue;
            if (strcmp(e->d_name, today) == 0) continue;
            if (oldest[0] == '\0' || strcmp(e->d_name, oldest) < 0)
                strlcpy(oldest, e->d_name, sizeof(oldest));
        }
        closedir(d);

        if (!oldest[0]) {
            ESP_LOGW(TAG, "retention: SD at %u%% (cap %u%%) but no prunable day-folder left",
                     used_pct, g_settings.sd_cap_pct);
            break;
        }

        remove_day_folder(oldest);
        ESP_LOGI(TAG, "retention: pruned /captures/%s (SD was at %u%%)", oldest, used_pct);
        esp_vfs_fat_info(STORAGE_MOUNT_POINT, &total, &free_bytes);
        if (total == 0) break;
    }
    xSemaphoreGive(s_write_mtx);
}

esp_err_t storage_save_jpeg(const uint8_t *data, size_t len,
                            char *path_out, size_t path_out_len)
{
    if (!s_sd_present) return ESP_ERR_INVALID_STATE;

    /* Build /sd/captures/<day>/<name>.jpg. Before the first SNTP sync the
     * clock reads ~1970 (FSD §3.4) — use an uptime-based name under no-date/
     * instead of a bogus timestamp. */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char day[32], name[40];
    if (tm_now.tm_year + 1900 < 2020) {
        strlcpy(day, "no-date", sizeof(day));
        snprintf(name, sizeof(name), "up%llu", (unsigned long long) (esp_timer_get_time() / 1000));
    } else {
        /* date-time-ms: an event's frames are ~500 ms apart, so second-only
         * names collided (only the b..z fallback below saved them) — the
         * millisecond field makes each frame's name naturally unique, which
         * best-of-N species ID (§3.2) relies on to re-read distinct frames. */
        strftime(day,  sizeof(day),  "%Y-%m-%d",          &tm_now);
        strftime(name, sizeof(name), "%Y-%m-%d_%H-%M-%S", &tm_now);
        size_t nl = strlen(name);
        snprintf(name + nl, sizeof(name) - nl, "-%03d", (int) (tv.tv_usec / 1000));
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
    bool ok = f && written == len;
#if SD_USE_SDMMC
    if (!ok && sd_recover()) {          /* transient write error → remount + retry once */
        mkdir(dir, 0775);
        f = fopen(path, "wb");
        written = f ? fwrite(data, 1, len, f) : 0;
        if (f) fclose(f);
        ok = f && written == len;
    }
#endif
    xSemaphoreGive(s_write_mtx);

    s_last_write_ok = ok;
    if (!ok) {
        ESP_LOGE(TAG, "write failed: %s (%u/%u bytes)", path,
                 (unsigned) written, (unsigned) len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "saved %s (%u bytes)", path, (unsigned) len);
    if (path_out)
        strlcpy(path_out, path + strlen(STORAGE_MOUNT_POINT), path_out_len);
    prune_if_over_cap();
    return ESP_OK;
}

/* Visit-log path for a day: /log/visits-<date>.csv, where `date` is the full
 * "YYYY-MM-DD" (only the first 10 chars are used, so a "YYYY-MM-DDT..." timestamp
 * works too) or the literal "no-date" for pre-SNTP captures. Per-day files (FSD
 * v2.07) so gallery/relabel/stats-today touch one day, not a whole month. */
void storage_visit_log_path(const char *date, char *buf, size_t sz)
{
    if (date && strcmp(date, "no-date") == 0)
        strlcpy(buf, STORAGE_MOUNT_POINT "/log/visits-no-date.csv", sz);
    else
        snprintf(buf, sz, STORAGE_MOUNT_POINT "/log/visits-%.10s.csv",
                 date ? date : "");
}

/* One-time migration (FSD v2.07): split any legacy monthly `visits-YYYY-MM.csv`
 * into per-day `visits-YYYY-MM-DD.csv`. Crash-safe and idempotent:
 *   - only touches files named exactly "visits-<7 chars>.csv" that aren't
 *     "no-date" (per-day names are 10 chars → skipped; re-running after all
 *     months are done is a no-op);
 *   - before splitting a month it deletes that month's per-day files (undoes a
 *     crashed prior run), so a retry is clean;
 *   - the monthly file is renamed to `.bak` (same-dir extension swap — never a
 *     cross-dir move, per the FATFS rename gotcha) ONLY after the per-day row
 *     count matches, so data is never lost; a mismatch keeps the `.csv` and
 *     retries next boot. `.bak` has no ".csv" substring so every enumerator
 *     ignores it. Captures under /captures are never touched. */
static void storage_migrate_perday(void)
{
    if (!s_sd_present) return;

    char months[64][8];   /* "YYYY-MM" + NUL */
    int  nm = 0;
    DIR *d = opendir(STORAGE_MOUNT_POINT "/log");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nm < 64) {
        if (e->d_type != DT_REG) continue;
        if (strncmp(e->d_name, "visits-", 7) != 0) continue;
        const char *mid = e->d_name + 7;
        const char *dot = strstr(mid, ".csv");
        if (!dot || (size_t) (dot - mid) != 7) continue;   /* not YYYY-MM (per-day=10) */
        if (strncmp(mid, "no-date", 7) == 0) continue;     /* keep the unsynced bucket */
        memcpy(months[nm], mid, 7); months[nm][7] = '\0';
        nm++;
    }
    closedir(d);
    if (nm == 0) return;   /* already per-day */

    xSemaphoreTake(s_write_mtx, portMAX_DELAY);
    for (int i = 0; i < nm; i++) {
        char mpath[64];
        snprintf(mpath, sizeof(mpath), STORAGE_MOUNT_POINT "/log/visits-%s.csv", months[i]);

        /* (1) undo any partial per-day files for this month from a crashed run */
        char pfx[24];
        snprintf(pfx, sizeof(pfx), "visits-%.7s-", months[i]);   /* "visits-YYYY-MM-" */
        DIR *dd = opendir(STORAGE_MOUNT_POINT "/log");
        if (dd) {
            struct dirent *de;
            while ((de = readdir(dd)) != NULL) {
                if (strncmp(de->d_name, pfx, strlen(pfx)) == 0 && strstr(de->d_name, ".csv")) {
                    char p[96];
                    snprintf(p, sizeof(p), STORAGE_MOUNT_POINT "/log/%.40s", de->d_name);
                    unlink(p);
                }
            }
            closedir(dd);
        }

        /* (2) split rows into per-day files (verbatim — ROI column untouched) */
        FILE *in = fopen(mpath, "r");
        if (!in) continue;
        char line[400];
        bool header = true;
        char cur[11] = "";
        FILE *out = NULL;
        long in_rows = 0, out_rows = 0;
        while (fgets(line, sizeof(line), in)) {
            if (header) {
                header = false;
                /* Skip the header row — but only if it IS one. A file without a
                 * header (unexpected) must keep its first data row, not lose it
                 * (ROI-preservation guarantee). */
                if (strncmp(line, "timestamp,", 10) == 0) continue;
            }
            if (line[0] == '\0' || line[0] == '\n') continue;
            in_rows++;
            char date[11];
            memcpy(date, line, 10); date[10] = '\0';
            const char *dst = (date[4] == '-' && date[7] == '-') ? date : "no-date";
            if (strcmp(dst, cur) != 0) {
                if (out) fclose(out);
                char dp[64];
                storage_visit_log_path(dst, dp, sizeof(dp));
                struct stat st;
                bool isnew = (stat(dp, &st) != 0);
                out = fopen(dp, "a");
                if (out && isnew)
                    fputs("timestamp,species,confidence,frames,first_frame,corrected,latin,roi,top3\n", out);
                strlcpy(cur, dst, sizeof(cur));
            }
            if (out && fputs(line, out) >= 0) out_rows++;
        }
        if (out) fclose(out);
        fclose(in);

        /* (3) verify + (4) back up (never delete). */
        if (out_rows == in_rows) {
            char bpath[64];
            snprintf(bpath, sizeof(bpath), STORAGE_MOUNT_POINT "/log/visits-%s.bak", months[i]);
            unlink(bpath);
            if (rename(mpath, bpath) == 0)
                ESP_LOGI(TAG, "migrate: %s -> per-day (%ld rows); monthly kept as .bak",
                         months[i], out_rows);
            else
                ESP_LOGE(TAG, "migrate: %s split ok but .bak rename failed — kept .csv", months[i]);
        } else {
            ESP_LOGE(TAG, "migrate: %s row mismatch (in=%ld out=%ld) — kept .csv, retry next boot",
                     months[i], in_rows, out_rows);
        }
    }
    xSemaphoreGive(s_write_mtx);
}

/* Append one CSV line (writing the header first if the file is new). Factored so
 * both the first attempt and the post-remount retry share it. Caller holds the
 * write mutex. */
static bool append_visit_line(const char *path, const char *line)
{
    struct stat st;
    bool is_new = (stat(path, &st) != 0);
    FILE *f = fopen(path, "a");
    if (!f) return false;
    if (is_new)
        fputs("timestamp,species,confidence,frames,first_frame,corrected,latin,roi,top3\n", f);
    bool ok = (fputs(line, f) >= 0) && (fputc('\n', f) != EOF);
    fclose(f);
    return ok;
}

esp_err_t storage_append_visit_log(const char *line)
{
    if (!s_sd_present) return ESP_ERR_INVALID_STATE;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char path[64];
    if (tm_now.tm_year + 1900 < 2020) {
        storage_visit_log_path("no-date", path, sizeof(path));
    } else {
        char date[11];
        /* unsigned + modulo so -Werror=format-truncation can prove each width
         * (year<10000 → 4 digits, month/day<100 → 2, all non-negative). */
        snprintf(date, sizeof(date), "%04u-%02u-%02u",
                 (unsigned) (tm_now.tm_year + 1900) % 10000u,
                 (unsigned) (tm_now.tm_mon + 1) % 100u,
                 (unsigned) tm_now.tm_mday % 100u);
        storage_visit_log_path(date, path, sizeof(path));
    }

    xSemaphoreTake(s_write_mtx, portMAX_DELAY);
    bool ok = append_visit_line(path, line);
#if SD_USE_SDMMC
    if (!ok && sd_recover()) ok = append_visit_line(path, line);
#endif
    xSemaphoreGive(s_write_mtx);

    s_last_write_ok = ok;
    if (!ok) ESP_LOGE(TAG, "visit log write failed: %s", path);
    return ok ? ESP_OK : ESP_FAIL;
}

int storage_reset_stats(void)
{
    if (!s_sd_present) return 0;

    xSemaphoreTake(s_write_mtx, portMAX_DELAY);
    int deleted = 0;
    DIR *d = opendir(STORAGE_MOUNT_POINT "/log");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_type != DT_REG) continue;
            if (strncmp(e->d_name, "visits-", 7) != 0) continue;
            if (!strstr(e->d_name, ".csv")) continue;
            char path[sizeof(STORAGE_MOUNT_POINT "/log/") + sizeof(e->d_name)];
            snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/log/%s", e->d_name);
            if (unlink(path) == 0) deleted++;
        }
        closedir(d);
    }
    xSemaphoreGive(s_write_mtx);

    ESP_LOGI(TAG, "stats reset: deleted %d visit-log file(s)", deleted);
    return deleted;
}

int storage_reset_stats_day(const char *date)
{
    if (!s_sd_present || !date || !date[0]) return 0;

    xSemaphoreTake(s_write_mtx, portMAX_DELAY);
    int removed = 0;

    /* Unsynced captures all live in one file — wipe it whole. */
    if (strcmp(date, "no-date") == 0) {
        if (unlink(STORAGE_MOUNT_POINT "/log/visits-no-date.csv") == 0) removed = 1;
        xSemaphoreGive(s_write_mtx);
        ESP_LOGI(TAG, "stats reset (no-date): removed unsynced log");
        return removed;
    }

    /* Real date -> filter that month's CSV, dropping rows whose timestamp
     * (first field, "YYYY-MM-DDT...") starts with this day. */
    char path[64], tmp[72];
    storage_visit_log_path(date, path, sizeof(path));
    snprintf(tmp,  sizeof(tmp),  STORAGE_MOUNT_POINT "/log/wipe.tmp");

    FILE *in = fopen(path, "r");
    if (!in) { xSemaphoreGive(s_write_mtx); return 0; }
    FILE *out = fopen(tmp, "w");
    if (!out) { fclose(in); xSemaphoreGive(s_write_mtx);
        ESP_LOGE(TAG, "stats day reset: temp open failed"); return 0; }

    /* Sized past the longest possible row (~330 with roi/top3) — an fgets
     * split would misjudge the row's tail as a separate (kept) line. */
    char line[400];
    bool header = true;
    int kept = 0;
    while (fgets(line, sizeof(line), in)) {
        if (header) { fputs(line, out); header = false; continue; }
        if (line[0] == '\0' || line[0] == '\n') continue;
        if (strncmp(line, date, 10) == 0) { removed++; continue; }  /* this day */
        fputs(line, out);
        kept++;
    }
    fclose(in);
    fclose(out);

    unlink(path);
    if (kept > 0) rename(tmp, path);   /* else the month is now empty — drop it */
    else          unlink(tmp);
    xSemaphoreGive(s_write_mtx);

    ESP_LOGI(TAG, "stats reset (%s): removed %d row(s), kept %d", date, removed, kept);
    return removed;
}

esp_err_t storage_set_roi(const char *date, const char *file, const char *roi)
{
    if (!s_sd_present || !date || !date[0] || !file || !file[0] || !roi)
        return ESP_ERR_INVALID_ARG;

    /* CSV-safe: a comma/newline in the roi would shift or break columns. The
     * caller validates the shape; this just guarantees the field can't corrupt
     * the row. */
    char rv[24];
    strlcpy(rv, roi, sizeof(rv));
    for (char *s = rv; *s; s++) { if (*s == ',') *s = ';'; else if (*s=='\n'||*s=='\r') *s=' '; }

    char path[64], tmp[72];
    storage_visit_log_path(date, path, sizeof(path));
    snprintf(tmp,  sizeof(tmp),  STORAGE_MOUNT_POINT "/log/relabel.tmp");

    xSemaphoreTake(s_write_mtx, portMAX_DELAY);
    FILE *in = fopen(path, "r");
    if (!in) { xSemaphoreGive(s_write_mtx); return ESP_ERR_NOT_FOUND; }
    FILE *out = fopen(tmp, "w");
    if (!out) { fclose(in); xSemaphoreGive(s_write_mtx);
        ESP_LOGE(TAG, "set_roi: temp open failed"); return ESP_FAIL; }

    bool found = false;
    char line[400];
    bool header = true;
    while (fgets(line, sizeof(line), in)) {
        if (header) { fputs(line, out); header = false; continue; }
        if (line[0] == '\0' || line[0] == '\n') continue;
        line[strcspn(line, "\r\n")] = '\0';
        char *fld[9] = {0};
        int nf = 0;
        char *p = line;
        fld[nf++] = p;
        while (nf < 9 && (p = strchr(p, ',')) != NULL) { *p++ = '\0'; fld[nf++] = p; }
        const char *ff  = nf > 4 ? fld[4] : "";
        const char *base = strrchr(ff, '/');
        base = base ? base + 1 : ff;
        if (!found && strcmp(base, file) == 0) {
            found = true;                /* roi(7) replaced; all else kept */
            fprintf(out, "%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                    nf>0?fld[0]:"", nf>1?fld[1]:"", nf>2?fld[2]:"", nf>3?fld[3]:"",
                    ff, nf>5?fld[5]:"", nf>6?fld[6]:"", rv, nf>8?fld[8]:"");
        } else {
            for (int i = 0; i < nf; i++) { if (i) fputc(',', out); fputs(fld[i], out); }
            fputc('\n', out);
        }
    }
    fclose(in);
    fclose(out);

    if (found) { unlink(path); rename(tmp, path); }
    else        unlink(tmp);
    xSemaphoreGive(s_write_mtx);
    ESP_LOGI(TAG, "set_roi %s/%s -> %s (%s)", date, file, rv, found ? "ok" : "no-row");
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* Accept the model's existing classification as human-confirmed (§3.4/v1.59):
 * copy the row's species(1) into the corrected(5) column, keeping its latin(6).
 * Only a real, not-yet-confirmed classification is confirmable — a row with an
 * empty latin (sentinel "no bird"/"Unidentified") or one already corrected is
 * left untouched and ESP_ERR_NOT_FOUND is returned so the caller can report it.
 * Unlike storage_relabel this never adds a row: there's nothing to confirm on an
 * image with no visit row. */
esp_err_t storage_confirm(const char *date, const char *file)
{
    if (!s_sd_present || !date || !date[0] || !file || !file[0])
        return ESP_ERR_INVALID_ARG;

    char path[64], tmp[72];
    storage_visit_log_path(date, path, sizeof(path));
    snprintf(tmp,  sizeof(tmp),  STORAGE_MOUNT_POINT "/log/relabel.tmp");

    xSemaphoreTake(s_write_mtx, portMAX_DELAY);
    FILE *in = fopen(path, "r");
    if (!in) { xSemaphoreGive(s_write_mtx); return ESP_ERR_NOT_FOUND; }
    FILE *out = fopen(tmp, "w");
    if (!out) { fclose(in); xSemaphoreGive(s_write_mtx);
        ESP_LOGE(TAG, "confirm: temp open failed"); return ESP_FAIL; }

    bool found = false;
    char line[400];
    bool header = true;
    while (fgets(line, sizeof(line), in)) {
        if (header) { fputs(line, out); header = false; continue; }
        if (line[0] == '\0' || line[0] == '\n') continue;
        line[strcspn(line, "\r\n")] = '\0';
        char *fld[9] = {0};
        int nf = 0;
        char *p = line;
        fld[nf++] = p;
        while (nf < 9 && (p = strchr(p, ',')) != NULL) { *p++ = '\0'; fld[nf++] = p; }
        const char *ff  = nf > 4 ? fld[4] : "";
        const char *base = strrchr(ff, '/');
        base = base ? base + 1 : ff;
        const char *species = nf > 1 ? fld[1] : "";
        const char *corr    = nf > 5 ? fld[5] : "";
        const char *latin   = nf > 6 ? fld[6] : "";
        if (!found && strcmp(base, file) == 0 && !corr[0] && latin[0]) {
            found = true;                /* corrected(5)=species(1); latin(6) kept */
            fprintf(out, "%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                    nf>0?fld[0]:"", species, nf>2?fld[2]:"", nf>3?fld[3]:"",
                    ff, species, latin, nf>7?fld[7]:"", nf>8?fld[8]:"");
        } else {
            for (int i = 0; i < nf; i++) { if (i) fputc(',', out); fputs(fld[i], out); }
            fputc('\n', out);
        }
    }
    fclose(in);
    fclose(out);

    if (found) { unlink(path); rename(tmp, path); }
    else        unlink(tmp);
    xSemaphoreGive(s_write_mtx);
    ESP_LOGI(TAG, "confirm %s/%s -> %s", date, file, found ? "ok" : "nothing-to-confirm");
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ── Per-frame motion-ROI sidecar (§3.4) ─────────────────────────────────────
 * capture.c logs every saved frame's re-detected box here so a later confirm of
 * a follow-up frame (which never had its own visit row) can recover that frame's
 * own box — the frames all exist, only the boxes were being discarded. */
esp_err_t storage_log_frame_roi(const char *date, const char *file, const char *roi)
{
    if (!s_sd_present || !date || strlen(date) != 10 || !file || !file[0] ||
        !roi || !roi[0] || strchr(file, ',') || strchr(file, '\n'))
        return ESP_ERR_INVALID_ARG;
    char path[64];
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/log/frameroi-%.10s.csv", date);
    xSemaphoreTake(s_write_mtx, portMAX_DELAY);
    FILE *f = fopen(path, "a");
    esp_err_t err = ESP_FAIL;
    if (f) { fprintf(f, "%s,%s\n", file, roi); fclose(f); err = ESP_OK; }
    xSemaphoreGive(s_write_mtx);
    return err;
}

/* Load a day's frame-ROI sidecar into a PSRAM buffer (caller frees), NUL-
 * terminated, or NULL if absent/empty. Takes the write lock briefly, so call it
 * BEFORE the relabel paths take it — otherwise they'd self-deadlock. */
static char *frameroi_load(const char *date)
{
    if (!date || strlen(date) < 10) return NULL;
    char path[64];
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/log/frameroi-%.10s.csv", date);
    char *buf = NULL;
    xSemaphoreTake(s_write_mtx, portMAX_DELAY);
    FILE *f = fopen(path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 2 * 1024 * 1024) {
            buf = heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (buf) { size_t rd = fread(buf, 1, sz, f); buf[rd] = '\0'; }
        }
        fclose(f);
    }
    xSemaphoreGive(s_write_mtx);
    return buf;
}

/* Find `file`'s recorded box ("x0-y0-x1-y1") in a loaded sidecar buffer, newest
 * wins is irrelevant (a frame appears once). Returns false if absent/empty. */
static bool frameroi_find(const char *buf, const char *file, char *out, size_t outsz)
{
    if (!buf || !file || !file[0]) return false;
    size_t flen = strlen(file);
    for (const char *p = buf; p && *p; ) {
        if (strncmp(p, file, flen) == 0 && p[flen] == ',') {
            const char *roi = p + flen + 1;
            size_t rl = strcspn(roi, "\r\n");
            if (rl > 0 && rl < outsz) { memcpy(out, roi, rl); out[rl] = '\0'; return true; }
            return false;
        }
        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : NULL;
    }
    return false;
}

esp_err_t storage_relabel(const char *date, const char *file,
                          const char *common, const char *latin)
{
    if (!s_sd_present || !date || !date[0] || !file || !file[0])
        return ESP_ERR_INVALID_ARG;

    /* CSV-safe copies: a comma would add phantom columns, a newline break the
     * row. Same sanitizing the classifier applies to written species names. */
    char c[64], l[64];
    strlcpy(c, common ? common : "", sizeof(c));
    strlcpy(l, latin  ? latin  : "", sizeof(l));
    for (char *s = c; *s; s++) { if (*s == ',') *s = ';'; else if (*s=='\n'||*s=='\r') *s=' '; }
    for (char *s = l; *s; s++) { if (*s == ',') *s = ';'; else if (*s=='\n'||*s=='\r') *s=' '; }

    char path[64], tmp[72];
    storage_visit_log_path(date, path, sizeof(path));
    snprintf(tmp,  sizeof(tmp),  STORAGE_MOUNT_POINT "/log/relabel.tmp");

    char *frbuf = frameroi_load(date);   /* per-frame ROI sidecar, before the lock */
    xSemaphoreTake(s_write_mtx, portMAX_DELAY);
    FILE *in  = fopen(path, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); xSemaphoreGive(s_write_mtx); free(frbuf);
        ESP_LOGE(TAG, "relabel: temp open failed"); return ESP_FAIL; }

    bool found = false;
    if (in) {
        char line[400];
        bool header = true;
        while (fgets(line, sizeof(line), in)) {
            if (header) { fputs(line, out); header = false; continue; }
            if (line[0] == '\0' || line[0] == '\n') continue;
            line[strcspn(line, "\r\n")] = '\0';
            /* Split into up to 9 fields — commas are pure delimiters here, every
             * value field sanitizes its own commas to ';' (see classify.cpp). */
            char *fld[9] = {0};
            int nf = 0;
            char *p = line;
            fld[nf++] = p;
            while (nf < 9 && (p = strchr(p, ',')) != NULL) { *p++ = '\0'; fld[nf++] = p; }
            const char *ff = nf > 4 ? fld[4] : "";
            const char *base = strrchr(ff, '/');
            base = base ? base + 1 : ff;
            if (!found && strcmp(base, file) == 0) {
                found = true;                /* corrected(5)+latin(6) replaced; model's
                                              * species(1) kept for reference */
                fprintf(out, "%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                        nf>0?fld[0]:"", nf>1?fld[1]:"", nf>2?fld[2]:"",
                        nf>3?fld[3]:"", ff, c, l, nf>7?fld[7]:"", nf>8?fld[8]:"");
            } else {
                for (int i = 0; i < nf; i++) { if (i) fputc(',', out); fputs(fld[i], out); }
                fputc('\n', out);
            }
        }
        fclose(in);
    } else {
        fputs("timestamp,species,confidence,frames,first_frame,corrected,latin,roi,top3\n", out);
    }

    if (!found) {
        /* Image has no visit row (not an event's first frame, or its row was
         * wiped) — add one, timestamp parsed from the capture filename
         * ("YYYY-MM-DD_HH-MM-SS-mmm.jpg"), marked user-confirmed (§3.4/v1.51). */
        char ts[20];
        if (strncmp(file, date, 10) == 0 && strlen(file) >= 20 && file[10] == '_')
            snprintf(ts, sizeof(ts), "%.10sT%.2s:%.2s:%.2s", file, file+11, file+14, file+17);
        else
            strlcpy(ts, date, sizeof(ts));
        char frroi[24] = "";
        frameroi_find(frbuf, file, frroi, sizeof(frroi));   /* this frame's own box, if logged */
        fprintf(out, "%s,%s,0,1,/captures/%.10s/%s,%s,%s,%s,\n", ts, c, date, file, c, l, frroi);
    }
    fclose(out);

    unlink(path);
    rename(tmp, path);
    xSemaphoreGive(s_write_mtx);
    free(frbuf);
    ESP_LOGI(TAG, "relabel %s/%s -> '%s' (%s)", date, file, c, found ? "updated" : "added");
    return ESP_OK;
}

esp_err_t storage_relabel_batch(const char *date, const char *const *files,
                                int nfiles, const char *common, const char *latin,
                                int *applied)
{
    if (applied) *applied = 0;
    if (!s_sd_present || !date || strlen(date) < 10 || !files || nfiles <= 0)
        return ESP_ERR_INVALID_ARG;

    /* Same CSV-safety as storage_relabel: a comma would add phantom columns, a
     * newline break the row. Sanitize the shared label once. */
    char c[64], l[64];
    strlcpy(c, common ? common : "", sizeof(c));
    strlcpy(l, latin  ? latin  : "", sizeof(l));
    for (char *s = c; *s; s++) { if (*s == ',') *s = ';'; else if (*s=='\n'||*s=='\r') *s=' '; }
    for (char *s = l; *s; s++) { if (*s == ',') *s = ';'; else if (*s=='\n'||*s=='\r') *s=' '; }

    /* One found-flag per target so unmatched images get a fresh row appended
     * (mirrors storage_relabel's no-row case), and so each image is applied to
     * at most one row. */
    bool *found = calloc(nfiles, sizeof(bool));
    if (!found) return ESP_ERR_NO_MEM;

    char path[64], tmp[72];
    storage_visit_log_path(date, path, sizeof(path));
    snprintf(tmp,  sizeof(tmp),  STORAGE_MOUNT_POINT "/log/relabel.tmp");

    char *frbuf = frameroi_load(date);   /* per-frame ROI sidecar, before the lock */
    xSemaphoreTake(s_write_mtx, portMAX_DELAY);
    FILE *in  = fopen(path, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); xSemaphoreGive(s_write_mtx); free(found); free(frbuf);
        ESP_LOGE(TAG, "relabel batch: temp open failed"); return ESP_FAIL; }

    int applied_n = 0;
    if (in) {
        char line[400];
        bool header = true;
        while (fgets(line, sizeof(line), in)) {
            if (header) { fputs(line, out); header = false; continue; }
            if (line[0] == '\0' || line[0] == '\n') continue;
            line[strcspn(line, "\r\n")] = '\0';
            char *fld[9] = {0};
            int nf = 0;
            char *p = line;
            fld[nf++] = p;
            while (nf < 9 && (p = strchr(p, ',')) != NULL) { *p++ = '\0'; fld[nf++] = p; }
            const char *ff = nf > 4 ? fld[4] : "";
            const char *base = strrchr(ff, '/');
            base = base ? base + 1 : ff;
            int hit = -1;
            for (int i = 0; i < nfiles; i++)
                if (!found[i] && files[i] && strcmp(base, files[i]) == 0) { hit = i; break; }
            if (hit >= 0) {
                found[hit] = true; applied_n++;   /* corrected(5)+latin(6) replaced */
                fprintf(out, "%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                        nf>0?fld[0]:"", nf>1?fld[1]:"", nf>2?fld[2]:"",
                        nf>3?fld[3]:"", ff, c, l, nf>7?fld[7]:"", nf>8?fld[8]:"");
            } else {
                for (int i = 0; i < nf; i++) { if (i) fputc(',', out); fputs(fld[i], out); }
                fputc('\n', out);
            }
        }
        fclose(in);
    } else {
        fputs("timestamp,species,confidence,frames,first_frame,corrected,latin,roi,top3\n", out);
    }

    /* Images with no existing row get one appended, user-confirmed, timestamp
     * parsed from the capture filename — same as storage_relabel's added case. */
    for (int i = 0; i < nfiles; i++) {
        if (found[i] || !files[i] || !files[i][0]) continue;
        const char *file = files[i];
        char ts[20];
        if (strncmp(file, date, 10) == 0 && strlen(file) >= 20 && file[10] == '_')
            snprintf(ts, sizeof(ts), "%.10sT%.2s:%.2s:%.2s", file, file+11, file+14, file+17);
        else
            strlcpy(ts, date, sizeof(ts));
        char frroi[24] = "";
        frameroi_find(frbuf, file, frroi, sizeof(frroi));   /* this frame's own box, if logged */
        fprintf(out, "%s,%s,0,1,/captures/%.10s/%s,%s,%s,%s,\n", ts, c, date, file, c, l, frroi);
        applied_n++;
    }
    fclose(out);

    unlink(path);
    rename(tmp, path);
    xSemaphoreGive(s_write_mtx);
    free(found);
    free(frbuf);
    if (applied) *applied = applied_n;
    ESP_LOGI(TAG, "relabel batch %s: %d image(s) -> '%s'", date, applied_n, c);
    return ESP_OK;
}
