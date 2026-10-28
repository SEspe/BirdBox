#pragma once
#include <stdint.h>
#include "esp_err.h"

/* Visit statistics (FSD §3.4): aggregates the monthly visit-log CSVs
 * (/sd/log/visits-*.csv) for the /api/stats endpoints. The device only
 * serves aggregated data; charts are rendered client-side (FSD §3.4). */

#include "stats_core.h"   /* stats_t and the row folding (pure, unit-tested) */
#include "best_core.h"    /* best_t: top visits per species (pure, unit-tested) */

/* Best visits per species (Stats tab, "Best visits", §3.4): the two best visits
 * of each species in one calendar year (`year`, 0 = all time), over the COMPLETE
 * visit-log history — not the 400-file window of stats_collect. `out` is filled
 * even when there is no SD (empty). Cached on the visit-log counters and the
 * year, so an unchanged log costs a copy. */
esp_err_t stats_best_visits(best_t *out, int year);


/* One image reference for the per-row image list (FSD §3.4/v1.50). */
typedef struct { char path[64]; char ts[20]; } stats_img_t;

/* Fills `out` with up to `max` most-recent visit-log first_frame paths whose
 * decided species equals `want` (raw log species value; "no bird" for the
 * false-positive row). `date` ("YYYY-MM-DD") scopes to that single day's log
 * (the Stats "Today" view, v2.68); NULL/"" scans every log file (all-time).
 * Returns the count filled, newest first. */
int stats_list_images(const char *want, const char *date, stats_img_t *out, int max);

/* Fills *out from the visit logs; zero stats (not an error) when no SD or
 * no logs. ~2.6 kB — allocate on the heap, not an httpd stack.
 * `date` ("YYYY-MM-DD") scopes to that single per-day file (the Stats "Today"
 * view, FSD v2.07); NULL aggregates every log file (the all-time view).
 * `stats_collect` is the all-time convenience wrapper. */
esp_err_t stats_collect_scoped(stats_t *out, const char *date);
esp_err_t stats_collect(stats_t *out);

/* Creates the aggregate cache (v3.44) — call once at boot, before anything can
 * serve /api/stats or publish to Home Assistant. Without it every call scans. */
void stats_init(void);
