#include "test.h"
#include "stats_core.h"

#include <stdlib.h>

/* stats_ingest_line modifies the line in place, so feed it a fresh copy. */
static void feed(stats_t *st, const char *row, const char *reset)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", row);
    stats_ingest_line(st, buf, reset);
}

static stats_t *fresh(void) { return calloc(1, sizeof(stats_t)); }

static int sp_index(const stats_t *st, const char *name)
{
    for (int i = 0; i < st->sp_count; i++)
        if (strcmp(st->sp[i], name) == 0) return i;
    return -1;
}

static int day_index(const stats_t *st, const char *day)
{
    for (int i = 0; i < st->day_count; i++)
        if (strcmp(st->day[i], day) == 0) return i;
    return -1;
}

void test_stats_core(void)
{
    /* One ordinary visit lands in every bucket. */
    {
        stats_t *st = fresh();
        feed(st, "2026-10-04T07:15:00,Kjøttmeis,95,5,/captures/2026-10-04/a.jpg,,Parus major,,\n", "");
        CHECK_INT(st->total, 1);
        CHECK_INT(st->day_count, 1);
        CHECK_STR(st->day[0], "2026-10-04");
        CHECK_INT(st->day_n[0], 1);
        CHECK_INT(st->hour[7], 1);
        CHECK_INT(st->sp_count, 1);
        CHECK_STR(st->sp[0], "Kjøttmeis");
        CHECK_STR(st->sp_latin[0], "Parus major");
        CHECK_STR(st->sp_first[0], "2026-10-04T07:15:00");
        free(st);
    }
    /* The user's correction wins over the classifier's species. */
    {
        stats_t *st = fresh();
        feed(st, "2026-10-04T07:15:00,Blåmeis,60,5,/c/a.jpg,Kjøttmeis,Parus major,,\n", "");
        CHECK_INT(st->sp_count, 1);
        CHECK_STR(st->sp[0], "Kjøttmeis");
        free(st);
    }
    /* "no bird" is a false positive: its own count and first/last, never a visit. */
    {
        stats_t *st = fresh();
        feed(st, "2026-10-04T06:00:00,no bird,80,3,/c/a.jpg,,,,\n", "");
        feed(st, "2026-10-04T09:00:00,Kjøttmeis,90,3,/c/b.jpg,no bird,,,\n", "");   /* corrected to no bird */
        CHECK_INT(st->false_pos, 2);
        CHECK_INT(st->total, 0);
        CHECK_INT(st->sp_count, 0);
        CHECK_INT(st->day_count, 0);
        CHECK_STR(st->fp_first, "2026-10-04T06:00:00");
        CHECK_STR(st->fp_last, "2026-10-04T09:00:00");
        free(st);
    }
    /* Reset point: rows strictly before it are skipped; at or after are kept. */
    {
        stats_t *st = fresh();
        const char *reset = "2026-10-04T12:00:00";
        feed(st, "2026-10-04T11:59:59,Kjøttmeis,90,3,/c/a.jpg,,Parus major,,\n", reset);
        feed(st, "2026-10-04T12:00:00,Kjøttmeis,90,3,/c/b.jpg,,Parus major,,\n", reset);
        feed(st, "2026-10-05T08:00:00,Kjøttmeis,90,3,/c/c.jpg,,Parus major,,\n", reset);
        CHECK_INT(st->total, 2);
        stats_t *none = fresh();
        feed(none, "2020-01-01T00:00:00,Kjøttmeis,90,3,/c/a.jpg,,Parus major,,\n", NULL);
        feed(none, "2020-01-01T00:00:00,Kjøttmeis,90,3,/c/a.jpg,,Parus major,,\n", "");
        CHECK_INT(none->total, 2);                 /* NULL and "" both mean no reset */
        free(st); free(none);
    }
    /* A row from before the clock synced counts as a visit and a species, but
     * has no day or hour to put it in. */
    {
        stats_t *st = fresh();
        feed(st, "unsynced,Kjøttmeis,90,3,/captures/no-date/up123.jpg,,Parus major,,\n", "");
        CHECK_INT(st->total, 1);
        CHECK_INT(st->sp_count, 1);
        CHECK_INT(st->day_count, 0);
        int hours = 0;
        for (int h = 0; h < 24; h++) hours += st->hour[h];
        CHECK_INT(hours, 0);
        free(st);
    }
    /* Rows missing a timestamp or species are ignored, not counted. */
    {
        stats_t *st = fresh();
        feed(st, ",Kjøttmeis,90,3,/c/a.jpg,,,,\n", "");
        feed(st, "2026-10-04T07:00:00,,90,3,/c/a.jpg,,,,\n", "");
        feed(st, "\n", "");
        CHECK_INT(st->total, 0);
        CHECK_INT(st->false_pos, 0);
        free(st);
    }
    /* One species under two common names merges on the Latin binomial (v2.70);
     * first/last-seen follow ingestion order. */
    {
        stats_t *st = fresh();
        feed(st, "2026-10-01T08:00:00,Bokfink,90,3,/c/a.jpg,,Fringilla coelebs,,\n", "");
        feed(st, "2026-10-03T08:00:00,Common Chaffinch,90,3,/c/b.jpg,,Fringilla coelebs,,\n", "");
        CHECK_INT(st->sp_count, 1);
        CHECK_INT(st->sp_n[0], 2);
        CHECK_STR(st->sp_first[0], "2026-10-01T08:00:00");
        CHECK_STR(st->sp_last[0], "2026-10-03T08:00:00");
        free(st);
    }
    /* Rows without Latin (e.g. "unclassified") group by their raw name. */
    {
        stats_t *st = fresh();
        feed(st, "2026-10-01T08:00:00,unclassified,0,3,/c/a.jpg,,,,\n", "");
        feed(st, "2026-10-01T09:00:00,unclassified,0,3,/c/b.jpg,,,,\n", "");
        CHECK_INT(st->sp_count, 1);
        CHECK_INT(st->sp_n[sp_index(st, "unclassified")], 2);
        free(st);
    }
    /* CRLF rows: the Latin name must not carry a '\r'. */
    {
        stats_t *st = fresh();
        feed(st, "2026-10-04T07:15:00,Kjøttmeis,95,5,/c/a.jpg,,Parus major\r\n", "");
        CHECK_STR(st->sp_latin[0], "Parus major");
        free(st);
    }
    /* More days than buckets: the NEWEST STATS_MAX_DAYS days are kept,
     * whichever order the files are read in (v3.44 fix — it used to freeze on
     * the first 62 seen). 70 days from 2026-07-01. */
    for (int order = 0; order < 2; order++) {
        stats_t *st = fresh();
        for (int k = 0; k < 70; k++) {
            int d = order == 0 ? k : 69 - k;          /* ascending, then descending */
            int month = 7 + d / 31, dom = 1 + d % 31;
            char row[160];
            snprintf(row, sizeof(row),
                     "2026-%02d-%02dT08:00:00,Kjøttmeis,90,3,/c/a.jpg,,Parus major,,\n", month, dom);
            feed(st, row, "");
        }
        CHECK_INT(st->day_count, STATS_MAX_DAYS);
        CHECK_INT(st->total, 70);
        CHECK(day_index(st, "2026-07-01") < 0);       /* oldest evicted */
        CHECK(day_index(st, "2026-07-08") < 0);       /* 8th oldest evicted (70-62=8) */
        CHECK(day_index(st, "2026-07-09") >= 0);      /* 9th oldest kept */
        CHECK(day_index(st, "2026-09-08") >= 0);      /* newest kept (day index 69) */
        int sum = 0;
        for (int i = 0; i < st->day_count; i++) sum += st->day_n[i];
        CHECK_INT(sum, STATS_MAX_DAYS);
        free(st);
    }
    /* More species than rows: the extras still count as visits. */
    {
        stats_t *st = fresh();
        for (int k = 0; k < STATS_MAX_SPECIES + 6; k++) {
            char row[160];
            snprintf(row, sizeof(row), "2026-10-04T08:00:00,Sp%d,90,3,/c/a.jpg,,Genus sp%d,,\n", k, k);
            feed(st, row, "");
        }
        CHECK_INT(st->sp_count, STATS_MAX_SPECIES);
        CHECK_INT(st->total, STATS_MAX_SPECIES + 6);
        free(st);
    }
}
