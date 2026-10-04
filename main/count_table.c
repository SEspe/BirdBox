#include "count_table.h"

#include <string.h>

int cnt_table_find(const cnt_table_t *t, const char *day)
{
    for (int i = 0; i < t->used; i++)
        if (strcmp(t->e[i].day, day) == 0) return i;
    return -1;
}

void cnt_table_drop(cnt_table_t *t, int i)
{
    if (i < 0 || i >= t->used) return;
    t->e[i] = t->e[--t->used];       /* order does not matter */
}

bool cnt_table_put(cnt_table_t *t, const char *day, int n)
{
    if (t->used >= t->cap || strlen(day) >= sizeof(t->e[0].day)) return false;
    if (cnt_table_find(t, day) >= 0) return false;
    strcpy(t->e[t->used].day, day);
    t->e[t->used].n = n < 0 ? 0 : n;
    t->used++;
    return true;
}

void cnt_table_add(cnt_table_t *t, const char *day, int delta)
{
    int i = cnt_table_find(t, day);
    if (i < 0) return;
    t->e[i].n += delta;
    if (t->e[i].n < 0) t->e[i].n = 0;
}

bool cnt_walk_raced(uint32_t gen0, uint32_t gen_now, uint32_t seq0, uint32_t seq_now,
                    const char *last_save_day, const char *day)
{
    if (gen_now != gen0) return true;
    return seq_now != seq0 && strcmp(last_save_day, day) == 0;
}
