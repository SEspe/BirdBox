#include "csv_field.h"

#include <string.h>

char *csv_next_field(char **p)
{
    char *start = *p;
    char *comma = strchr(start, ',');
    if (comma) {
        *comma = '\0';
        *p = comma + 1;
    } else {
        char *end = start + strcspn(start, "\r\n");
        *end = '\0';
        *p = end;
    }
    return start;
}
