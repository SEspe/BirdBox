#pragma once
#include <stdint.h>

/* The pure part of the visit statistics (FSD §3.4): the aggregate type and
 * how one visit-log row is folded into it. No ESP-IDF headers, no SD, no
 * settings, so test/host can exercise it on a PC. stats.c does the file
 * walking and caching around it. */

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

/* Fold one visit-log row (timestamp,species,confidence,frames,first_frame,
 * corrected,latin,...) into *st. `line` is modified in place. Rows older than
 * `reset_ts` ("YYYY-MM-DDTHH:MM:SS", or "" for none) are skipped. */
void stats_ingest_line(stats_t *st, char *line, const char *reset_ts);
