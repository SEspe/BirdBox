#pragma once
#include <stdbool.h>
#include <stddef.h>

/* The operator's curated target species list (downloads Artsliste.txt) — the
 * species BirdBox actively works toward: hand-labelled in the relabel picker,
 * the set the Claude fallback is constrained to, and the mask for the optional
 * periodic iNat batch (FSD §3.2.1/§3.2.3). Kept in ONE place so those three
 * consumers can never drift apart.
 *
 * Each entry is the scientific binomial + English common name; the Norwegian
 * display name is looked up separately in species_i18n.c keyed on the binomial
 * (every binomial here has a NO_NAMES entry there). */
typedef struct { const char *latin; const char *common; } target_species_t;

extern const target_species_t TARGET_SPECIES[];
extern const size_t TARGET_SPECIES_N;

/* True when `latin` (a scientific binomial, e.g. "Pica pica") is one of the
 * target species. Case-sensitive, exact match. */
bool species_in_target(const char *latin);
