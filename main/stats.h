#pragma once
#include <stdint.h>
#include "esp_err.h"

/* Visit statistics (FSD §3.4): aggregates the monthly visit-log CSVs
 * (/sd/log/visits-*.csv) for the /api/stats endpoints. The device only
 * serves aggregated data; charts are rendered client-side (FSD §3.4). */

#define STATS_MAX_DAYS     62   /* two months of daily buckets */
#define STATS_MAX_SPECIES  24
#define STATS_MAX_LOGFILES 12   /* newest-first cap on files parsed per request */

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
} stats_t;

/* Fills *out from the visit logs; zero stats (not an error) when no SD or
 * no logs. ~2.6 kB — allocate on the heap, not an httpd stack. */
esp_err_t stats_collect(stats_t *out);
