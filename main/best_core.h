#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Best visits per species (Stats tab, "Best visits" section). The pure part:
 * the per-species top-two selection and the visit-log row parsing. No ESP-IDF
 * headers, no SD, so test/host can exercise it on a PC. stats.c walks the
 * files and feeds the rows in.
 *
 * "Best" = human-confirmed first, then the classifier's confidence, then the
 * newer visit. */

#define BEST_MAX_SPECIES 64    /* species kept; further ones are counted in overflow */
#define BEST_PER_SPECIES 2     /* visits kept per species */
#define BEST_MAX_YEARS   32    /* distinct years listed for the year picker */

typedef struct {
    char    file[64];          /* first_frame web path, e.g. /captures/2026-10-28/x.jpg */
    char    ts[20];            /* ISO timestamp of the visit */
    uint8_t conf;              /* classifier confidence, 0-100 */
    bool    confirmed;         /* the visit has a user-confirmed label */
} best_visit_t;

typedef struct {
    char        name[32];      /* display name (the species column of the first row) */
    char        latin[40];     /* binomial when known, "" otherwise */
    uint16_t    n;             /* all visits of this species in the scope */
    char        first[20];     /* earliest synced visit, ISO; "" if none */
    char        last[20];      /* latest synced visit, ISO; "" if none */
    uint8_t     nv;            /* visits held in v[], 0..BEST_PER_SPECIES */
    best_visit_t v[BEST_PER_SPECIES];   /* best first */
} best_species_t;

typedef struct {
    int             sp_count;
    uint32_t        overflow;  /* species dropped because the table was full */
    best_species_t  sp[BEST_MAX_SPECIES];
    int             year_count;
    int             years[BEST_MAX_YEARS];   /* distinct years with logs, ascending */
} best_t;

void best_init(best_t *b);

/* True when visit a ranks above visit b (confirmed, then confidence, then newer). */
bool best_visit_better(const best_visit_t *a, const best_visit_t *b);

/* Fold one visit-log row (timestamp,species,confidence,frames,first_frame,
 * corrected,latin,...) into *b. `line` is modified in place. `year` 0 takes
 * every row; otherwise only rows whose timestamp is in that year. "no bird" and
 * "unclassified" rows are skipped, as are rows without a first_frame. */
void best_ingest_line(best_t *b, char *line, int year);

/* Records a year that has a visit log, for the picker (ignores duplicates). */
void best_add_year(best_t *b, int year);
