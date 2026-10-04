#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The pure part of the per-day capture-count cache (v3.45, storage.c): a small
 * table of day -> count, and the rule that decides whether an unlocked
 * directory walk may be stored. No ESP-IDF headers and no locking — storage.c
 * holds its mutex around every call — so test/host can test it on a PC. */

typedef struct { char day[16]; int n; } cnt_ent_t;
typedef struct { cnt_ent_t *e; int used; int cap; } cnt_table_t;

int  cnt_table_find(const cnt_table_t *t, const char *day);   /* index or -1 */
void cnt_table_drop(cnt_table_t *t, int i);                   /* i<0: no-op */
/* Insert day=n if the day is absent, there is room and the name fits.
 * Returns whether it was stored. Never overwrites an existing entry. */
bool cnt_table_put(cnt_table_t *t, const char *day, int n);
/* +delta to a cached day, clamped at 0; absent days are left absent (a later
 * walk counts them from the disk). */
void cnt_table_add(cnt_table_t *t, const char *day, int delta);

/* May a walk of `day` that began with (gen0, seq0) be stored now that the
 * counters read (gen_now, seq_now)? No if any delete/invalidation happened
 * (gen moved), or if a save happened AND the last save went to this day —
 * saves only ever go to the current day, so a save elsewhere cannot have
 * changed what this walk saw. */
bool cnt_walk_raced(uint32_t gen0, uint32_t gen_now, uint32_t seq0, uint32_t seq_now,
                    const char *last_save_day, const char *day);
