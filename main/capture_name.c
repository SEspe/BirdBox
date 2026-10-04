#include "capture_name.h"

#include <string.h>

bool capture_name_hour(const char *name, char out[3])
{
    if (strlen(name) < 14 || name[10] != '_' || name[13] != '-') return false;
    if (name[11] < '0' || name[11] > '9' || name[12] < '0' || name[12] > '9') return false;
    out[0] = name[11]; out[1] = name[12]; out[2] = '\0';
    return true;
}

int capture_name_hour_num(const char *name)
{
    char hh[3];
    if (!capture_name_hour(name, hh)) return -1;
    int h = (hh[0] - '0') * 10 + (hh[1] - '0');
    return (h >= 0 && h < 24) ? h : -1;
}

bool capture_path_split(const char *logical, char *day, size_t dsz,
                        char *name, size_t nsz)
{
    if (dsz == 0 || nsz == 0) return false;
    day[0] = name[0] = '\0';
    const char *p = strstr(logical, "/captures/");
    if (!p) return false;
    p += strlen("/captures/");
    const char *slash = strchr(p, '/');
    if (!slash || (size_t) (slash - p) >= dsz) return false;
    memcpy(day, p, (size_t) (slash - p));
    day[slash - p] = '\0';
    if (strlen(slash + 1) >= nsz) { day[0] = '\0'; return false; }   /* would truncate */
    strcpy(name, slash + 1);
    /* A name with a slash in it is already a physical, bucketed path — the
     * caller handed us something it got from the filesystem, not the log. */
    return day[0] && name[0] && !strchr(name, '/');
}
