#include "stats.h"
#include "storage.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>

#include "esp_log.h"

static const char *TAG = "stats";

/* Row format (storage_append_visit_log, FSD §3.4):
 * timestamp,species,confidence,frames,first_frame,corrected
 * A non-empty "corrected" column wins over "species" (user relabels, §3.2). */
static void ingest_line(stats_t *st, char *line)
{
    char *save = NULL;
    char *ts        = strtok_r(line, ",", &save);
    char *species   = strtok_r(NULL, ",", &save);
    strtok_r(NULL, ",", &save);                    /* confidence */
    strtok_r(NULL, ",", &save);                    /* frames */
    strtok_r(NULL, ",", &save);                    /* first_frame */
    char *corrected = strtok_r(NULL, ",\r\n", &save);

    if (!ts || !species) return;
    if (corrected && corrected[0]) species = corrected;
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
        char line[224];
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
