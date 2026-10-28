#include "best_core.h"
#include "csv_field.h"

#include <stdlib.h>
#include <string.h>

/* Row format (storage_append_visit_log, FSD §3.4), read positionally:
 * timestamp,species,confidence,frames,first_frame,corrected,latin,...
 * A non-empty "corrected" column is the user's label and marks the visit as
 * confirmed; the Latin name then belongs to the corrected species (v1.51). */

void best_init(best_t *b)
{
    memset(b, 0, sizeof(*b));
}

bool best_visit_better(const best_visit_t *a, const best_visit_t *b)
{
    if (a->confirmed != b->confirmed) return a->confirmed;
    if (a->conf != b->conf) return a->conf > b->conf;
    return strcmp(a->ts, b->ts) > 0;      /* ISO timestamps order chronologically */
}

/* Keeps v[] sorted best-first and holds at most BEST_PER_SPECIES entries. */
static void offer_visit(best_species_t *s, const best_visit_t *cand)
{
    int pos = s->nv;
    if (pos == BEST_PER_SPECIES) {
        if (!best_visit_better(cand, &s->v[BEST_PER_SPECIES - 1])) return;
        pos = BEST_PER_SPECIES - 1;       /* replace the weakest */
    } else {
        s->nv++;
    }
    while (pos > 0 && best_visit_better(cand, &s->v[pos - 1])) {
        s->v[pos] = s->v[pos - 1];
        pos--;
    }
    s->v[pos] = *cand;
}

static bool ts_in_year(const char *ts, int year)
{
    if (year == 0) return true;
    if (strlen(ts) < 4) return false;
    for (int i = 0; i < 4; i++)
        if (ts[i] < '0' || ts[i] > '9') return false;
    return atoi(ts) == year;               /* "unsynced" rows never match a year */
}

void best_ingest_line(best_t *b, char *line, int year)
{
    char *p = line;
    char *ts        = csv_next_field(&p);
    char *species   = csv_next_field(&p);
    char *conf      = csv_next_field(&p);
    csv_next_field(&p);                    /* frames */
    char *first     = csv_next_field(&p);
    char *corrected = csv_next_field(&p);
    char *latin     = csv_next_field(&p);

    if (!ts[0] || !species[0] || !first[0]) return;
    if (corrected[0]) species = corrected;
    if (strcmp(species, "no bird") == 0 || strcmp(species, "unclassified") == 0) return;
    if (!ts_in_year(ts, year)) return;

    /* Same identity rule as the Stats table: the Latin binomial when both rows
     * have one, else the common name. */
    int i;
    for (i = 0; i < b->sp_count; i++) {
        best_species_t *s = &b->sp[i];
        if (latin[0] && s->latin[0]) {
            if (strcmp(s->latin, latin) == 0) break;
        } else if (strcmp(s->name, species) == 0) {
            break;
        }
    }
    if (i == b->sp_count) {
        if (b->sp_count == BEST_MAX_SPECIES) { b->overflow++; return; }
        best_species_t *s = &b->sp[b->sp_count++];
        memset(s, 0, sizeof(*s));
        strlcpy(s->name, species, sizeof(s->name));
        strlcpy(s->latin, latin, sizeof(s->latin));
    }
    best_species_t *s = &b->sp[i];
    if (s->n < UINT16_MAX) s->n++;
    if (!s->latin[0] && latin[0]) strlcpy(s->latin, latin, sizeof(s->latin));
    /* First and last seen only from synced timestamps: "unsynced" would sort
     * after every date and falsely become the last visit. */
    if (ts[0] >= '0' && ts[0] <= '9') {
        if (!s->first[0] || strcmp(ts, s->first) < 0) strlcpy(s->first, ts, sizeof(s->first));
        if (!s->last[0] || strcmp(ts, s->last) > 0) strlcpy(s->last, ts, sizeof(s->last));
    }

    long c = conf[0] ? strtol(conf, NULL, 10) : 0;
    if (c < 0) c = 0;
    if (c > 100) c = 100;

    best_visit_t v;
    memset(&v, 0, sizeof(v));
    strlcpy(v.file, first, sizeof(v.file));
    strlcpy(v.ts, ts, sizeof(v.ts));
    v.conf = (uint8_t) c;
    v.confirmed = corrected[0] != '\0';
    offer_visit(s, &v);
}

void best_add_year(best_t *b, int year)
{
    if (year < 1970 || year > 2999) return;     /* refuse the pre-SNTP clock */
    for (int i = 0; i < b->year_count; i++)
        if (b->years[i] == year) return;
    if (b->year_count == BEST_MAX_YEARS) return;
    /* keep ascending */
    int pos = b->year_count;
    while (pos > 0 && b->years[pos - 1] > year) {
        b->years[pos] = b->years[pos - 1];
        pos--;
    }
    b->years[pos] = year;
    b->year_count++;
}
