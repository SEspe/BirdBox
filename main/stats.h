#pragma once
#include <stdint.h>
#include "esp_err.h"

/* Visit statistics (FSD §3.4): aggregates the monthly visit-log CSVs
 * (/sd/log/visits-*.csv) for the /api/stats endpoints. The device only
 * serves aggregated data; charts are rendered client-side (FSD §3.4). */

#define STATS_MAX_DAYS     62   /* two months of daily buckets */
#define STATS_MAX_SPECIES  24
#define STATS_MAX_LOGFILES 400  /* newest-first cap on files parsed per request.
                                 * v2.07: per-day files (not monthly), so this is
                                 * days (~13 months) not months; older history
                                 * beyond it is dropped from the all-time view
                                 * (acceptable — history need not be exact). */

typedef struct {
    int      day_count;
    char     day[STATS_MAX_DAYS][11];        /* "YYYY-MM-DD" */
    uint16_t day_n[STATS_MAX_DAYS];

    int      sp_count;
    char     sp[STATS_MAX_SPECIES][32];
    uint16_t sp_n[STATS_MAX_SPECIES];
    char     sp_first[STATS_MAX_SPECIES][20];  /* ISO timestamps */
    char     sp_last[STATS_MAX_SPECIES][20];
    char     sp_latin[STATS_MAX_SPECIES][40];  /* "" if unknown (older rows,
                                                   or a user-corrected label) */

    uint16_t hour[24];
    uint32_t total;

    /* Rows the classifier confidently decided were "no bird" (background
     * class at/above the confidence threshold): motion triggers confirmed as
     * false positives. Kept out of the bird species/daily/hourly buckets and
     * the visits total, but surfaced as their own row in the species table
     * (FSD §3.4/v1.50) — count + first/last, like a species line. */
    uint32_t false_pos;
    char     fp_first[20];
    char     fp_last[20];
} stats_t;

/* One image reference for the per-row image list (FSD §3.4/v1.50). */
typedef struct { char path[64]; char ts[20]; } stats_img_t;

/* Fills `out` with up to `max` most-recent visit-log first_frame paths whose
 * decided species equals `want` (raw log species value; "no bird" for the
 * false-positive row). `date` ("YYYY-MM-DD") scopes to that single day's log
 * (the Stats "Today" view, v2.68); NULL/"" scans every log file (all-time).
 * Returns the count filled, newest first. */
int stats_list_images(const char *want, const char *date, stats_img_t *out, int max);

/* Fills *out from the visit logs; zero stats (not an error) when no SD or
 * no logs. ~2.6 kB — allocate on the heap, not an httpd stack.
 * `date` ("YYYY-MM-DD") scopes to that single per-day file (the Stats "Today"
 * view, FSD v2.07); NULL aggregates every log file (the all-time view).
 * `stats_collect` is the all-time convenience wrapper. */
esp_err_t stats_collect_scoped(stats_t *out, const char *date);
esp_err_t stats_collect(stats_t *out);
