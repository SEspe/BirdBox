#include "test.h"
#include "csv_field.h"

void test_csv_field(void)
{
    /* The reason this scanner exists: an EMPTY field must survive. strtok
     * would merge ",," and shift every later column left by one. */
    {
        char line[] = "a,,c\n";
        char *p = line;
        CHECK_STR(csv_next_field(&p), "a");
        CHECK_STR(csv_next_field(&p), "");
        CHECK_STR(csv_next_field(&p), "c");
        CHECK_STR(csv_next_field(&p), "");      /* past the end: empty, stable */
        CHECK_STR(csv_next_field(&p), "");
    }
    /* A real visit-log row with the usual empty "corrected" column: the Latin
     * name must land in column 6, not slide into column 5. */
    {
        char line[] = "2026-10-04T08:15:00,Kjøttmeis,95,5,/captures/2026-10-04/x.jpg,,Parus major,1;2;3;4,\n";
        char *p = line;
        csv_next_field(&p); csv_next_field(&p); csv_next_field(&p);
        csv_next_field(&p); csv_next_field(&p);
        CHECK_STR(csv_next_field(&p), "");              /* corrected */
        CHECK_STR(csv_next_field(&p), "Parus major");   /* latin */
    }
    /* Line endings never leak into the last field. */
    {
        char crlf[] = "x,y\r\n";
        char *p = crlf;
        CHECK_STR(csv_next_field(&p), "x");
        CHECK_STR(csv_next_field(&p), "y");
        char bare[] = "x,y";
        p = bare;
        csv_next_field(&p);
        CHECK_STR(csv_next_field(&p), "y");
    }
    /* Trailing comma: one more, empty, field. Empty line: one empty field. */
    {
        char line[] = "a,b,\n";
        char *p = line;
        CHECK_STR(csv_next_field(&p), "a");
        CHECK_STR(csv_next_field(&p), "b");
        CHECK_STR(csv_next_field(&p), "");
        char empty[] = "\n";
        p = empty;
        CHECK_STR(csv_next_field(&p), "");
    }
}
