#include "stats.h"
#include "storage.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>

#include "esp_log.h"

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
        }
        if (i < st->day_count) st->day_n[i]++;

        int hh = (ts[11] - '0') * 10 + (ts[12] - '0');
        if (hh >= 0 && hh < 24) st->hour[hh]++;
    }

    int i;
    for (i = 0; i < st->sp_count; i++)
        if (strcmp(st->sp[i], species) == 0) break;
    if (i == st->sp_count && st->sp_count < STATS_MAX_SPECIES) {
        strlcpy(st->sp[st->sp_count], species, sizeof(st->sp[0]));
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

esp_err_t stats_collect(stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!storage_sd_present()) return ESP_OK;

    /* Collect matching log filenames, newest first (names sort by date) */
    char names[STATS_MAX_LOGFILES][40];
    int  n_files = 0;
    DIR *d = opendir(STORAGE_MOUNT_POINT "/log");
    if (!d) return ESP_OK;
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

    for (int f = 0; f < n_files; f++) {
        char path[64];
        snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/log/%.40s", names[f]);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        /* Sized past the longest possible row (~330 with roi/top3, v1.33): an
         * fgets split would surface the row's tail as a bogus extra row. */
        char line[400];
        bool header = true;
        while (fgets(line, sizeof(line), fp)) {
            if (header) { header = false; continue; }
            if (line[0] == '\0' || line[0] == '\n') continue;
            ingest_line(out, line);
        }
        fclose(fp);
    }

    ESP_LOGD(TAG, "stats: %lu visits, %d days, %d species",
             (unsigned long) out->total, out->day_count, out->sp_count);
    return ESP_OK;
}

int stats_list_images(const char *want, stats_img_t *out, int max)
{
    if (max <= 0 || !want || !want[0] || !storage_sd_present()) return 0;

    /* Log filenames, ascending (oldest first) so a single pass with a ring
     * buffer of size `max` naturally ends holding the newest `max` matches. */
    char names[STATS_MAX_LOGFILES][40];
    int  n_files = 0;
    DIR *d = opendir(STORAGE_MOUNT_POINT "/log");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_REG) continue;
        if (strncmp(e->d_name, "visits-", 7) != 0) continue;
        if (!strstr(e->d_name, ".csv")) continue;
        if (n_files < STATS_MAX_LOGFILES)
            strlcpy(names[n_files++], e->d_name, sizeof(names[0]));
    }
    closedir(d);
    for (int i = 1; i < n_files; i++) {          /* insertion sort ascending */
        char tmp[40];
        strlcpy(tmp, names[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], tmp) > 0) {
            strlcpy(names[j + 1], names[j], sizeof(names[0]));
            j--;
        }
        strlcpy(names[j + 1], tmp, sizeof(names[0]));
    }

    stats_img_t *ring = calloc(max, sizeof(stats_img_t));
    if (!ring) return 0;
    int total = 0;

    for (int f = 0; f < n_files; f++) {
        char path[64];
        snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/log/%.40s", names[f]);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        char line[400];
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
            next_field(&p);                    /* latin */
            if (!ts[0] || !species[0] || !first[0]) continue;
            if (corrected[0]) species = corrected;
            if (strcmp(species, want) != 0) continue;
            stats_img_t *slot = &ring[total % max];
            strlcpy(slot->path, first, sizeof(slot->path));
            strlcpy(slot->ts,   ts,    sizeof(slot->ts));
            total++;
        }
        fclose(fp);
    }

    int cnt = total < max ? total : max;
    for (int k = 0; k < cnt; k++) {          /* newest first */
        int idx = (total - 1 - k) % max;
        if (idx < 0) idx += max;
        out[k] = ring[idx];
    }
    free(ring);
    return cnt;
}
