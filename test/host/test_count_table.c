#include "test.h"
#include "count_table.h"

void test_count_table(void)
{
    cnt_ent_t ents[3];
    cnt_table_t t = { ents, 0, 3 };

    /* put / find */
    CHECK(cnt_table_put(&t, "2026-10-01", 100));
    CHECK(cnt_table_put(&t, "2026-10-02", 200));
    CHECK_INT(cnt_table_find(&t, "2026-10-01"), 0);
    CHECK_INT(cnt_table_find(&t, "2026-10-02"), 1);
    CHECK_INT(cnt_table_find(&t, "2026-10-03"), -1);
    CHECK(!cnt_table_put(&t, "2026-10-01", 5));           /* never overwrites */
    CHECK_INT(t.e[0].n, 100);

    /* add: saves +1, deletes -1, clamped at zero; absent days stay absent. */
    cnt_table_add(&t, "2026-10-01", +1);
    CHECK_INT(t.e[0].n, 101);
    cnt_table_add(&t, "2026-10-02", -250);
    CHECK_INT(t.e[1].n, 0);
    cnt_table_add(&t, "2026-10-09", +1);
    CHECK_INT(cnt_table_find(&t, "2026-10-09"), -1);
    CHECK_INT(t.used, 2);

    /* capacity and over-long names are refused, not overflowed */
    CHECK(cnt_table_put(&t, "no-date", 7));
    CHECK(!cnt_table_put(&t, "2026-10-04", 1));           /* full */
    cnt_ent_t one[1];
    cnt_table_t t2 = { one, 0, 1 };
    CHECK(!cnt_table_put(&t2, "a-name-that-is-far-too-long", 1));
    CHECK_INT(t2.used, 0);
    CHECK(cnt_table_put(&t2, "x", -5));
    CHECK_INT(t2.e[0].n, 0);                               /* negative stored as 0 */

    /* drop: swaps the last entry in; bad indexes are no-ops */
    cnt_table_drop(&t, cnt_table_find(&t, "2026-10-01"));
    CHECK_INT(t.used, 2);
    CHECK_INT(cnt_table_find(&t, "2026-10-01"), -1);
    CHECK(cnt_table_find(&t, "no-date") >= 0);
    CHECK(cnt_table_find(&t, "2026-10-02") >= 0);
    cnt_table_drop(&t, -1);
    cnt_table_drop(&t, 99);
    CHECK_INT(t.used, 2);

    /* The walk-race rule. */
    CHECK(!cnt_walk_raced(5, 5, 9, 9, "2026-10-04", "2026-10-04"));   /* nothing moved */
    CHECK(cnt_walk_raced(5, 6, 9, 9, "2026-10-04", "2026-10-01"));    /* any delete/invalidate */
    CHECK(cnt_walk_raced(5, 5, 9, 10, "2026-10-04", "2026-10-04"));   /* a save to THIS day */
    CHECK(!cnt_walk_raced(5, 5, 9, 10, "2026-10-04", "2026-10-01"));  /* saves went elsewhere */
}
