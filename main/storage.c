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
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "storage";

static bool              s_sd_present = false;
static sdmmc_card_t     *s_card = NULL;
static SemaphoreHandle_t s_write_mtx;
static bool              s_last_write_ok = true;

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

void storage_get_card_name(char *out, size_t out_len)
{
    if (!s_sd_present) { out[0] = '\0'; return; }
    strlcpy(out, s_card->cid.name, out_len);
}

bool storage_last_write_ok(void) { return s_last_write_ok; }

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
    xSemaphoreGive(s_write_mtx);

    s_last_write_ok = f && written == len;
    if (!s_last_write_ok) {
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

esp_err_t storage_append_visit_log(const char *line)
{
    if (!s_sd_present) return ESP_ERR_INVALID_STATE;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char path[64];
    if (tm_now.tm_year + 1900 < 2020)
        strlcpy(path, STORAGE_MOUNT_POINT "/log/visits-no-date.csv", sizeof(path));
    else
        snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/log/visits-%04d-%02d.csv",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1);

    xSemaphoreTake(s_write_mtx, portMAX_DELAY);
    struct stat st;
    bool is_new = (stat(path, &st) != 0);
    FILE *f = fopen(path, "a");
    bool ok = false;
    if (f) {
        if (is_new)
            fputs("timestamp,species,confidence,frames,first_frame,corrected,latin,roi,top3\n", f);
        ok = (fputs(line, f) >= 0) && (fputc('\n', f) != EOF);
        fclose(f);
    }
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
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/log/visits-%.7s.csv", date);
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
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/log/visits-%.7s.csv", date);
    snprintf(tmp,  sizeof(tmp),  STORAGE_MOUNT_POINT "/log/relabel.tmp");

    xSemaphoreTake(s_write_mtx, portMAX_DELAY);
    FILE *in  = fopen(path, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); xSemaphoreGive(s_write_mtx);
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
        fprintf(out, "%s,%s,0,1,/captures/%.10s/%s,%s,%s,,\n", ts, c, date, file, c, l);
    }
    fclose(out);

    unlink(path);
    rename(tmp, path);
    xSemaphoreGive(s_write_mtx);
    ESP_LOGI(TAG, "relabel %s/%s -> '%s' (%s)", date, file, c, found ? "updated" : "added");
    return ESP_OK;
}
