#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True when `latin` (a scientific binomial) is one of the Northern-European
 * garden/feeder/nestbox species — the same curated set this file uses for
 * Norwegian names doubles as the region allowlist for the §3.2.1 region
 * filter (single source of truth: one list to maintain). Empty/NULL → false. */
bool species_in_region(const char *latin);

/* Formats a display string for a classified species (FSD §3.2). `name_en`
 * is the English common name already stored in the visit log; `latin` is
 * the scientific binomial (empty for the "no bird"/"Unidentified bird"/
 * "unclassified" sentinels, which have no species identity to translate).
 * When lang is LANG_NO and a Norwegian name is known for that binomial, it
 * replaces the common name; the Latin name is always appended when known,
 * regardless of language, so the result is never ambiguous. */
void species_localize(const char *name_en, const char *latin,
                       species_lang_t lang, char *out, size_t outsz);

#ifdef __cplusplus
}
#endif
