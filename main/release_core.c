#include "release_core.h"

#include <stdio.h>
#include <string.h>

bool semver_parse(const char *s, int v[3])
{
    char tail;
    if (!s || !(s[0] >= '0' && s[0] <= '9')) return false;   /* no sign, no "v" */
    return sscanf(s, "%d.%d.%d%c", &v[0], &v[1], &v[2], &tail) == 3 &&
           v[0] >= 0 && v[1] >= 0 && v[2] >= 0;
}

bool semver_newer(const char *a, const char *b)
{
    int x[3], y[3];
    if (!semver_parse(a, x) || !semver_parse(b, y)) return false;
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] > y[i];
    return false;
}

bool release_extract_tag(const char *json, char *out, size_t n)
{
    if (!json || !out || n == 0) return false;
    out[0] = '\0';
    const char *p = strstr(json, "\"tag_name\"");
    if (!p) return false;
    p += strlen("\"tag_name\"");
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p++ != '"') return false;
    if (*p == 'v' || *p == 'V') p++;
    size_t i = 0;
    while (*p && *p != '"') {
        if (!((*p >= '0' && *p <= '9') || *p == '.')) { out[0] = '\0'; return false; }
        if (i + 1 >= n) { out[0] = '\0'; return false; }   /* would truncate */
        out[i++] = *p++;
    }
    out[i] = '\0';
    if (*p != '"' || i == 0) { out[0] = '\0'; return false; }
    return true;
}

bool release_has_bin(const char *json)
{
    const char *as = json ? strstr(json, "\"assets\"") : NULL;
    return as && strstr(as, ".bin\"") != NULL;
}
