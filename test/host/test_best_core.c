#include "test.h"
#include "best_core.h"

#include <stdlib.h>

/* best_ingest_line modifies the line in place, so feed it a fresh copy. */
static void feed(best_t *b, const char *row, int year)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", row);
    best_ingest_line(b, buf, year);
}

static best_t *fresh(void)
{
    best_t *b = calloc(1, sizeof(best_t));
    best_init(b);
    return b;
}

static int sp_index(const best_t *b, const char *name)
{
    for (int i = 0; i < b->sp_count; i++)
        if (strcmp(b->sp[i].name, name) == 0) return i;
    return -1;
}

void test_best_core(void)
{
    /* Ranking: confirmed beats a higher confidence; then confidence; then newer. */
    {
        best_visit_t conf91 = { "a.jpg", "2026-05-01T08:00:00", 91, false };
        best_visit_t conf60c = { "b.jpg", "2026-05-02T08:00:00", 60, true };
        best_visit_t conf91new = { "c.jpg", "2026-05-03T08:00:00", 91, false };
        CHECK(best_visit_better(&conf60c, &conf91));
        CHECK(!best_visit_better(&conf91, &conf60c));
        CHECK(best_visit_better(&conf91new, &conf91));    /* same conf: newer wins */
        CHECK(!best_visit_better(&conf91, &conf91new));
    }

    /* Top two per species: a third, weaker visit is dropped; a stronger one displaces the weakest. */
    {
        best_t *b = fresh();
        feed(b, "2026-05-01T08:00:00,Blåmeis (Cyanistes caeruleus),70,3,/captures/a.jpg,,Cyanistes caeruleus", 0);
        feed(b, "2026-05-02T08:00:00,Blåmeis (Cyanistes caeruleus),80,3,/captures/b.jpg,,Cyanistes caeruleus", 0);
        feed(b, "2026-05-03T08:00:00,Blåmeis (Cyanistes caeruleus),50,3,/captures/c.jpg,,Cyanistes caeruleus", 0);
        int i = sp_index(b, "Blåmeis (Cyanistes caeruleus)");
        CHECK(i >= 0);
        CHECK_INT(b->sp[i].n, 3);
        CHECK_INT(b->sp[i].nv, 2);
        CHECK_STR(b->sp[i].v[0].file, "/captures/b.jpg");   /* 80 */
        CHECK_STR(b->sp[i].v[1].file, "/captures/a.jpg");   /* 70; the 50 was dropped */

        feed(b, "2026-05-04T08:00:00,Blåmeis (Cyanistes caeruleus),95,3,/captures/d.jpg,,Cyanistes caeruleus", 0);
        CHECK_INT(b->sp[i].n, 4);
        CHECK_STR(b->sp[i].v[0].file, "/captures/d.jpg");   /* 95 enters at the top, 70 is pushed out */
        CHECK_STR(b->sp[i].v[1].file, "/captures/b.jpg");   /* 80 */
        free(b);
    }

    /* A confirmed visit ranks above an unconfirmed one of higher confidence. */
    {
        best_t *b = fresh();
        feed(b, "2026-05-01T08:00:00,Bokfink,99,3,/captures/high.jpg,,Fringilla coelebs", 0);
        feed(b, "2026-05-02T08:00:00,Bokfink,40,3,/captures/conf.jpg,Bokfink,Fringilla coelebs", 0);
        CHECK_STR(b->sp[0].v[0].file, "/captures/conf.jpg");
        CHECK(b->sp[0].v[0].confirmed);
        CHECK(!b->sp[0].v[1].confirmed);
        free(b);
    }

    /* Year scope: only that year's rows; unsynced rows only in all-time. */
    {
        best_t *b = fresh();
        feed(b, "2025-12-31T23:00:00,Gråspurv,60,3,/captures/2025.jpg,,Passer domesticus", 2025);
        feed(b, "2026-01-01T08:00:00,Gråspurv,60,3,/captures/2026.jpg,,Passer domesticus", 2026);
        feed(b, "unsynced,Gråspurv,60,3,/captures/nodate.jpg,,Passer domesticus", 2026);
        CHECK_INT(b->sp[0].n, 1);                       /* 2026 row only; unsynced never matches a year */
        CHECK_STR(b->sp[0].v[0].file, "/captures/2026.jpg");
        free(b);

        b = fresh();
        feed(b, "2025-12-31T23:00:00,Gråspurv,60,3,/captures/2025.jpg,,Passer domesticus", 0);
        feed(b, "unsynced,Gråspurv,60,3,/captures/nodate.jpg,,Passer domesticus", 0);
        CHECK_INT(b->sp[0].n, 2);                       /* all-time keeps both */
        free(b);
    }

    /* "no bird", "unclassified" and rows without a frame are not species. */
    {
        best_t *b = fresh();
        feed(b, "2026-05-01T08:00:00,no bird,80,3,/captures/nb.jpg,,", 0);
        feed(b, "2026-05-01T08:00:00,unclassified,0,3,/captures/u.jpg,,", 0);
        feed(b, "2026-05-01T08:00:00,Gråspurv,80,3,,,Passer domesticus", 0);
        CHECK_INT(b->sp_count, 0);
        free(b);
    }

    /* Identity: the Latin binomial joins two common names for one species. */
    {
        best_t *b = fresh();
        feed(b, "2026-05-01T08:00:00,Bokfink,70,3,/captures/n.jpg,,Fringilla coelebs", 0);
        feed(b, "2026-05-02T08:00:00,Common Chaffinch,75,3,/captures/e.jpg,,Fringilla coelebs", 0);
        CHECK_INT(b->sp_count, 1);
        CHECK_INT(b->sp[0].n, 2);
        CHECK_STR(b->sp[0].latin, "Fringilla coelebs");
        free(b);
    }

    /* First and last seen: earliest and latest synced visit; unsynced rows never set them. */
    {
        best_t *b = fresh();
        feed(b, "2026-05-02T08:00:00,Gråspurv,60,3,/captures/m.jpg,,Passer domesticus", 0);
        feed(b, "2026-04-20T08:00:00,Gråspurv,60,3,/captures/a.jpg,,Passer domesticus", 0);
        feed(b, "2026-06-01T08:00:00,Gråspurv,60,3,/captures/z.jpg,,Passer domesticus", 0);
        feed(b, "unsynced,Gråspurv,60,3,/captures/u.jpg,,Passer domesticus", 0);
        CHECK_STR(b->sp[0].first, "2026-04-20T08:00:00");
        CHECK_STR(b->sp[0].last, "2026-06-01T08:00:00");
        free(b);
    }

    /* Table full: further species are counted as overflow, not stored. */
    {
        best_t *b = fresh();
        char row[160];
        for (int k = 0; k < BEST_MAX_SPECIES + 3; k++) {
            snprintf(row, sizeof(row), "2026-05-01T08:00:00,Art %d,50,3,/captures/%d.jpg,,Latin %d", k, k, k);
            feed(b, row, 0);
        }
        CHECK_INT(b->sp_count, BEST_MAX_SPECIES);
        CHECK_INT(b->overflow, 3);
        free(b);
    }

    /* Years: ascending, unique, pre-SNTP years refused. */
    {
        best_t *b = fresh();
        best_add_year(b, 2026);
        best_add_year(b, 2024);
        best_add_year(b, 2026);
        best_add_year(b, 1970 - 1);
        best_add_year(b, 2025);
        CHECK_INT(b->year_count, 3);
        CHECK_INT(b->years[0], 2024);
        CHECK_INT(b->years[1], 2025);
        CHECK_INT(b->years[2], 2026);
        free(b);
    }
}
