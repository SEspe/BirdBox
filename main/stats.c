#include "stats.h"
#include "storage.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "stats";

/* Row format (storage_append_visit_log, FSD §3.4):
 * timestamp,species,confidence,frames,first_frame,corrected,latin,roi,top3
 * The trailing roi/top3 columns (v1.33, field-tuning data) are ignored here —
 * fields are read positionally and parsing stops at latin, so rows written
 * before those columns existed parse identically.
 * A non-empty "corrected" column wins over "species" (user relabels, §3.2).
 * "latin" is empty on rows written before that column existed, same as
 * "unknown". A corrected label has no matching latin name, so it's
 * dropped rather than misattributed to the original (uncorrected)
 * species' binomial.
 *
 * Fields are split by hand rather than with strtok_r: the "corrected"
 * column is always empty today (no relabeling UI yet), and strtok_r
 * treats runs of adjacent delimiters as one separator — it would silently
 * skip that empty field and misread "latin" as "corrected" instead. */
static char *next_field(char **p)
{
    char *start = *p;
    char *comma = strchr(start, ',');
    if (comma) {
        *comma = '\0';
        *p = comma + 1;
    } else {
        char *end = start + strcspn(start, "\r\n");
        *end = '\0';
        *p = end;
    }
    return start;
}

static void ingest_line(stats_t *st, char *line)
{
    char *p = line;
    char *ts        = next_field(&p);
    char *species   = next_field(&p);
    next_field(&p);                    /* confidence */
    next_field(&p);                    /* frames */
    next_field(&p);                    /* first_frame */
    char *corrected = next_field(&p);
    char *latin     = next_field(&p);

    if (!ts[0] || !species[0]) return;
    /* Stats are "since the last reset" (§3.4): skip rows older than the stored
     * reset epoch. ISO "YYYY-MM-DDT..." timestamps order lexicographically, so a
     * strcmp is a correct chronological compare. The visit log itself is never
     * deleted by a reset, so this filters counting only — labels/ROIs persist. */
    if (g_settings.stats_reset_ts[0] && strcmp(ts, g_settings.stats_reset_ts) < 0)
        return;
    /* User-confirmed label wins (§3.2/§3.4). Since v1.51 the relabel writes the
     * corrected species' own binomial into the latin column, so keep it (was
     * blanked when corrected labels had no known latin) — it localizes right. */
    if (corrected[0]) species = corrected;

    /* Confirmed false positive (classifier said "no bird" at/above the
     * threshold, §3.2): a motion trigger, not a bird visit. Kept out of the
     * bird buckets (species/daily/hourly/total) so wind events don't pollute
     * them, but tracked with its own count + first/last so the species table
     * can show it as an equal row (§3.4/v1.50). first/last mirror the species
     * rows' convention: first-encountered is kept, last-encountered updates. */
    if (strcmp(species, "no bird") == 0) {
        st->false_pos++;
        if (!st->fp_first[0]) strlcpy(st->fp_first, ts, sizeof(st->fp_first));
        strlcpy(st->fp_last, ts, sizeof(st->fp_last));
        return;
    }
    st->total++;

    /* Daily + hourly buckets need a synced timestamp ("YYYY-MM-DDTHH:...");
     * "unsynced" rows still count toward species totals. */
    if (strlen(ts) >= 13 && ts[4] == '-' && ts[10] == 'T') {
        char day[11];
        memcpy(day, ts, 10);
        day[10] = '\0';
        int i;
        for (i = 0; i < st->day_count; i++)
            if (strcmp(st->day[i], day) == 0) break;
        if (i == st->day_count && st->day_count < STATS_MAX_DAYS) {
            strlcpy(st->day[st->day_count], day, sizeof(st->day[0]));
            st->day_count++;
        } else if (i == st->day_count) {
            /* Full. The chart shows the NEWEST days, so a newer day evicts the
             * oldest bucket. Keeping the first 62 seen (before v3.44) froze
             * the chart once a card held more than 62 days of logs. */
            int old = 0;
            for (int k = 1; k < st->day_count; k++)
                if (strcmp(st->day[k], st->day[old]) < 0) old = k;
            if (strcmp(day, st->day[old]) > 0) {
                strlcpy(st->day[old], day, sizeof(st->day[0]));
                st->day_n[old] = 0;
                i = old;
            }
        }
        if (i < st->day_count) st->day_n[i]++;

        int hh = (ts[11] - '0') * 10 + (ts[12] - '0');
        if (hh >= 0 && hh < 24) st->hour[hh]++;
    }

    /* One species can be logged under different common names — Norwegian from
     * the relabel vocabulary, English from the cloud tier ("Bokfink" vs
     * "Common Chaffinch") — which used to split it into duplicate rows. The
     * Latin binomial is the species' identity whenever both sides have one
     * (v2.70); the raw name only decides for rows without Latin
     * ("unclassified", pre-v1.51 legacy rows). */
    int i;
    for (i = 0; i < st->sp_count; i++) {
        if (latin && latin[0] && st->sp_latin[i][0]) {
            if (strcmp(st->sp_latin[i], latin) == 0) break;
        } else if (strcmp(st->sp[i], species) == 0) {
            break;
        }
    }
    if (i == st->sp_count && st->sp_count < STATS_MAX_SPECIES) {
        strlcpy(st->sp[st->sp_count], species, sizeof(st->sp[0]));
        if (latin && latin[0])
            strlcpy(st->sp_latin[st->sp_count], latin, sizeof(st->sp_latin[0]));
        strlcpy(st->sp_first[st->sp_count], ts, sizeof(st->sp_first[0]));
        st->sp_count++;
    }
    if (i < st->sp_count) {
        st->sp_n[i]++;
        strlcpy(st->sp_last[i], ts, sizeof(st->sp_last[0]));
        if (latin && latin[0] && !st->sp_latin[i][0])
            strlcpy(st->sp_latin[i], latin, sizeof(st->sp_latin[0]));
    }
}

/* stdio's default buffer on this VFS is tiny, so fgets() over a ~130 kB day log
 * turns into thousands of small FATFS reads. A 16 kB buffer makes it a few
 * dozen. PSRAM: this is a transient read buffer and internal DRAM is the
 * scarce pool. The caller frees *vbuf after fclose(). */
#define STATS_READ_BUF (16 * 1024)
static FILE *open_log(const char *fname, char **vbuf)
{
    char path[64];
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/log/%.40s", fname);
    FILE *fp = fopen(path, "r");
    *vbuf = NULL;
    if (!fp) return NULL;
    *vbuf = heap_caps_malloc(STATS_READ_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (*vbuf) setvbuf(fp, *vbuf, _IOFBF, STATS_READ_BUF);
    return fp;
}

/* Read one visit-log file and ingest its rows. */
static void stats_ingest_file(stats_t *out, const char *fname)
{
    char *vbuf;
    FILE *fp = open_log(fname, &vbuf);
    if (!fp) return;
    /* Sized past the longest possible row (~330 with roi/top3, v1.33): an
     * fgets split would surface the row's tail as a bogus extra row. */
    char line[768];
    bool header = true;
    while (fgets(line, sizeof(line), fp)) {
        if (header) { header = false; continue; }
        if (line[0] == '\0' || line[0] == '\n') continue;
        ingest_line(out, line);
    }
    fclose(fp);
    free(vbuf);
}

static int name_cmp(const void *a, const void *b) { return strcmp(a, b); }

/* The newest STATS_MAX_LOGFILES visit-log names, sorted ascending, i.e. oldest
 * day first, because the names are visits-YYYY-MM-DD.csv. Sorted rather than
 * left in readdir order: FAT reuses freed directory slots, so readdir order is
 * only usually chronological, and first/last-seen mean first/last ingested.
 * Returns the count; *names_out is PSRAM and the caller frees it. */
static int list_logs(char (**names_out)[40])
{
    char (*names)[40] = heap_caps_malloc(STATS_MAX_LOGFILES * sizeof(names[0]),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    *names_out = names;
    if (!names) return 0;
    int n_files = 0;
    DIR *d = opendir(STORAGE_MOUNT_POINT "/log");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_REG) continue;
        if (strncmp(e->d_name, "visits-", 7) != 0) continue;
        if (!strstr(e->d_name, ".csv")) continue;
        if (n_files < STATS_MAX_LOGFILES) {
            strlcpy(names[n_files], e->d_name, sizeof(names[0]));
            n_files++;
        } else {
            /* keep the lexicographically largest (newest) names */
            int min = 0;
            for (int i = 1; i < n_files; i++)
                if (strcmp(names[i], names[min]) < 0) min = i;
            if (strcmp(e->d_name, names[min]) > 0)
                strlcpy(names[min], e->d_name, sizeof(names[0]));
        }
    }
    closedir(d);
    qsort(names, n_files, sizeof(names[0]), name_cmp);
    return n_files;
}

/* Ingest every log file except `skip` (a bare filename, or NULL). */
static void ingest_all(stats_t *out, const char *skip)
{
    char (*names)[40];
    int n = list_logs(&names);
    for (int f = 0; f < n; f++)
        if (!skip || strcmp(names[f], skip) != 0)
            stats_ingest_file(out, names[f]);
    free(names);
}

static void day_file(const char *date, char *buf, size_t sz)
{
    char path[64];
    storage_visit_log_path(date, path, sizeof(path));
    const char *base = strrchr(path, '/');
    strlcpy(buf, base ? base + 1 : path, sz);
}

/* ── The cache (v3.44) ───────────────────────────────────────────────────
 * The Stats tab asks for daily, species and hourly in one go, and each used to
 * re-read EVERY visit log: three identical ~2.2 s scans on a 15-day card, run
 * one after another because httpd serves one request at a time, so 6.7 s
 * before the tab could draw. Home Assistant runs the same scan every 15 min.
 *
 * Past days only change when something REWRITES a log (relabel, confirm,
 * recheck, a reset); appends only ever go to today's file. So the aggregate of
 * every day before today is kept (`hist`) and rebuilt only when
 * storage_visit_log_gen() moves, the date rolls over, or the stats reset point
 * changes. A request then costs a copy plus a read of today's file. On top of
 * that the finished result (`full`) is reused until anything at all is
 * appended, which makes the second and third request of one tab load free.
 * `day` does the same for the single-day view.
 *
 * One mutex serialises builders, so a request that arrives mid-build waits for
 * the result instead of starting its own scan. The counters are read BEFORE the
 * files: a write that lands mid-build leaves the slot keyed to the old counter,
 * and the next request rebuilds. */
typedef struct {
    bool     valid;
    uint32_t gen, appends;
    char     date[11];                               /* today, or the day scoped */
    char     reset[sizeof(g_settings.stats_reset_ts)];
    stats_t  st;
} stats_slot_t;

static SemaphoreHandle_t s_lock;
static stats_slot_t *s_hist, *s_full, *s_day;

void stats_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_hist = heap_caps_calloc(1, sizeof(stats_slot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_full = heap_caps_calloc(1, sizeof(stats_slot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_day  = heap_caps_calloc(1, sizeof(stats_slot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_lock || !s_hist || !s_full || !s_day)
        ESP_LOGW(TAG, "stats cache unavailable; every request will scan");
}

static bool slot_ok(const stats_slot_t *s, uint32_t gen, const uint32_t *appends,
                    const char *date)
{
    return s->valid && s->gen == gen && (!appends || s->appends == *appends) &&
           strcmp(s->date, date) == 0 &&
           strcmp(s->reset, g_settings.stats_reset_ts) == 0;
}

static void slot_key(stats_slot_t *s, uint32_t gen, uint32_t appends, const char *date)
{
    s->gen = gen;
    s->appends = appends;
    strlcpy(s->date, date, sizeof(s->date));
    strlcpy(s->reset, g_settings.stats_reset_ts, sizeof(s->reset));
    s->valid = true;
}

/* Today's "YYYY-MM-DD", or false on the pre-SNTP clock: then appends go to
 * visits-no-date.csv, which the past-days cache would not know to watch. */
static bool today_str(char *buf, size_t sz)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (tmv.tm_year + 1900 < 2020) return false;
    strftime(buf, sz, "%Y-%m-%d", &tmv);
    return true;
}

esp_err_t stats_collect_scoped(stats_t *out, const char *date)
{
    memset(out, 0, sizeof(*out));
    if (!storage_sd_present()) return ESP_OK;

    char today[11];
    bool cache = s_lock && s_hist && s_full && s_day && today_str(today, sizeof(today));
    char fname[48];

    /* Single day (v2.07): one small read, cached until that file can change. */
    if (date && date[0]) {
        day_file(date, fname, sizeof(fname));
        if (!cache) { stats_ingest_file(out, fname); return ESP_OK; }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t gen = storage_visit_log_gen(), app = storage_visit_log_appends();
        if (!slot_ok(s_day, gen, &app, date)) {
            memset(&s_day->st, 0, sizeof(s_day->st));
            stats_ingest_file(&s_day->st, fname);
            slot_key(s_day, gen, app, date);
        }
        *out = s_day->st;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    if (!cache) { ingest_all(out, NULL); return ESP_OK; }

    day_file(today, fname, sizeof(fname));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t gen = storage_visit_log_gen(), app = storage_visit_log_appends();
    if (!slot_ok(s_full, gen, &app, today)) {
        if (!slot_ok(s_hist, gen, NULL, today)) {
            int64_t t0 = esp_timer_get_time();
            memset(&s_hist->st, 0, sizeof(s_hist->st));
            ingest_all(&s_hist->st, fname);
            slot_key(s_hist, gen, 0, today);
            ESP_LOGI(TAG, "stats: past days rebuilt in %lld ms",
                     (esp_timer_get_time() - t0) / 1000);
        }
        /* Today last, so first/last-seen keep their oldest-first meaning. */
        s_full->st = s_hist->st;
        stats_ingest_file(&s_full->st, fname);
        slot_key(s_full, gen, app, today);
    }
    *out = s_full->st;
    xSemaphoreGive(s_lock);

    ESP_LOGD(TAG, "stats: %lu visits, %d days, %d species",
             (unsigned long) out->total, out->day_count, out->sp_count);
    return ESP_OK;
}

esp_err_t stats_collect(stats_t *out) { return stats_collect_scoped(out, NULL); }

int stats_list_images(const char *want, const char *date, stats_img_t *out, int max)
{
    if (max <= 0 || !want || !want[0] || !storage_sd_present()) return 0;

    /* Log filenames, ascending (oldest first) so a single pass with a ring
     * buffer of size `max` naturally ends holding the newest `max` matches.
     * Heap-allocated (the cap is days-scale since v2.07, too big for the stack). */
    char (*names)[40] = heap_caps_malloc(STATS_MAX_LOGFILES * sizeof(names[0]),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!names) return 0;
    int  n_files = 0;
    if (date && date[0]) {
        /* Day scope (v2.68): only that day's log file — mirrors the Today
         * branch of stats_collect_scoped, so the images match the table. */
        char path[64];
        storage_visit_log_path(date, path, sizeof(path));
        const char *base = strrchr(path, '/');
        strlcpy(names[n_files++], base ? base + 1 : path, sizeof(names[0]));
    } else {
    free(names);
    n_files = list_logs(&names);
    if (!names) return 0;
    }

    stats_img_t *ring = calloc(max, sizeof(stats_img_t));
    if (!ring) { free(names); return 0; }
    int total = 0;

    for (int f = 0; f < n_files; f++) {
        char *vbuf;
        FILE *fp = open_log(names[f], &vbuf);
        if (!fp) continue;
        char line[768];
        bool header = true;
        while (fgets(line, sizeof(line), fp)) {
            if (header) { header = false; continue; }
            if (line[0] == '\0' || line[0] == '\n') continue;
            char *p = line;
            char *ts        = next_field(&p);
            char *species   = next_field(&p);
            next_field(&p);                    /* confidence */
            next_field(&p);                    /* frames */
            char *first     = next_field(&p);
            char *corrected = next_field(&p);
            char *latin     = next_field(&p);
            if (!ts[0] || !species[0] || !first[0]) continue;
            if (corrected[0]) species = corrected;
            /* `want` is the species-table row key: the Latin binomial when the
             * row has one (v2.70), else the raw common name — match either, so
             * a merged row ("Bokfink" + "Common Chaffinch") lists all its
             * images whichever name each event was logged under. */
            if (strcmp(species, want) != 0 && strcmp(latin, want) != 0) continue;
            stats_img_t *slot = &ring[total % max];
            strlcpy(slot->path, first, sizeof(slot->path));
            strlcpy(slot->ts,   ts,    sizeof(slot->ts));
            total++;
        }
        fclose(fp);
        free(vbuf);
    }

    int cnt = total < max ? total : max;
    for (int k = 0; k < cnt; k++) {          /* newest first */
        int idx = (total - 1 - k) % max;
        if (idx < 0) idx += max;
        out[k] = ring[idx];
    }
    free(ring);
    free(names);
    return cnt;
}
