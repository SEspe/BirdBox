#include "test.h"
#include "capture_name.h"

void test_capture_name(void)
{
    char hh[3];

    /* The hour comes from the fixed offset in the capture's own name. */
    CHECK(capture_name_hour("2026-10-04_17-48-45-345.jpg", hh));
    CHECK_STR(hh, "17");
    CHECK_INT(capture_name_hour_num("2026-10-04_17-48-45-345.jpg"), 17);
    CHECK_INT(capture_name_hour_num("2026-10-04_00-00-00-000.jpg"), 0);
    CHECK_INT(capture_name_hour_num("2026-10-04_23-59-59-999.jpg"), 23);
    CHECK_INT(capture_name_hour_num("2026-10-04_17-48-45-345b.jpg"), 17);   /* same-ms suffix */

    /* No hour: pre-SNTP names, short names, wrong separators, non-digits. */
    CHECK(!capture_name_hour("up123456.jpg", hh));
    CHECK_INT(capture_name_hour_num("up123456.jpg"), -1);
    CHECK(!capture_name_hour("2026-10-04", hh));
    CHECK(!capture_name_hour("2026-10-04-17-48-45.jpg", hh));
    CHECK(!capture_name_hour("2026-10-04_1x-48-45-345.jpg", hh));
    CHECK(!capture_name_hour("", hh));
    /* Two digits but not an hour: shaped right, so a bucket name, but no number. */
    CHECK(capture_name_hour("2026-10-04_25-00-00-000.jpg", hh));
    CHECK_INT(capture_name_hour_num("2026-10-04_25-00-00-000.jpg"), -1);

    /* Logical path split. */
    char day[16], name[40];
    CHECK(capture_path_split("/captures/2026-10-04/2026-10-04_17-48-45-345.jpg",
                             day, sizeof(day), name, sizeof(name)));
    CHECK_STR(day, "2026-10-04");
    CHECK_STR(name, "2026-10-04_17-48-45-345.jpg");
    CHECK(capture_path_split("/sd/captures/no-date/up123.jpg", day, sizeof(day), name, sizeof(name)));
    CHECK_STR(day, "no-date");
    CHECK_STR(name, "up123.jpg");

    /* A physical bucketed path is not a logical one. */
    CHECK(!capture_path_split("/captures/2026-10-04/17/x.jpg", day, sizeof(day), name, sizeof(name)));
    /* Missing pieces. */
    CHECK(!capture_path_split("/captures/2026-10-04/", day, sizeof(day), name, sizeof(name)));
    CHECK(!capture_path_split("/captures/2026-10-04", day, sizeof(day), name, sizeof(name)));
    CHECK(!capture_path_split("/captures//x.jpg", day, sizeof(day), name, sizeof(name)));
    CHECK(!capture_path_split("/log/visits-2026-10-04.csv", day, sizeof(day), name, sizeof(name)));
    /* Too long for the caller's buffers: refused, never truncated into a
     * different (wrong) file name. */
    char tiny_day[5];
    CHECK(!capture_path_split("/captures/2026-10-04/x.jpg", tiny_day, sizeof(tiny_day), name, sizeof(name)));
    char tiny_name[8];
    CHECK(!capture_path_split("/captures/2026-10-04/2026-10-04_17-48-45-345.jpg",
                              day, sizeof(day), tiny_name, sizeof(tiny_name)));
}
