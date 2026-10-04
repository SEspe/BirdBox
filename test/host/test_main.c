/* Host unit tests for BirdBox's pure firmware logic (see README.md here).
 * Build and run:  make -C test/host test */
#include "test.h"

int g_checks, g_failures;

int main(void)
{
    struct { const char *name; void (*fn)(void); } suites[] = {
        { "csv_field",    test_csv_field    },
        { "stats_core",   test_stats_core   },
        { "release_core", test_release_core },
        { "capture_name", test_capture_name },
        { "count_table",  test_count_table  },
    };
    for (size_t i = 0; i < sizeof(suites) / sizeof(suites[0]); i++) {
        int before = g_failures, checks = g_checks;
        suites[i].fn();
        printf("%-14s %4d checks  %s\n", suites[i].name, g_checks - checks,
               g_failures == before ? "ok" : "FAILED");
    }
    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
