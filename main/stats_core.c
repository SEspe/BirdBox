#include "stats_core.h"
#include "csv_field.h"

#include <string.h>

/* Row format (storage_append_visit_log, FSD §3.4):
 * timestamp,species,confidence,frames,first_frame,corrected,latin,roi,top3
 * The trailing roi/top3 columns (v1.33, field-tuning data) are ignored here —
 * fields are read positionally and parsing stops at latin, so rows written
 * before those columns existed parse identically.
 * A non-empty "corrected" column wins over "species" (user relabels, §3.2).
 * "latin" is empty on rows written before that column existed, same as
 * "unknown". A corrected label has no matching latin name, so it's
 * dropped rather than misattributed to the original (uncorrected)
 * species' binomial.
 *
 * Fields are split by hand rather than with strtok_r: the "corrected"
 * column is always empty today (no relabeling UI yet), and strtok_r
 * treats runs of adjacent delimiters as one separator — it would silently
 * skip that empty field and misread "latin" as "corrected" instead. */
void stats_ingest_line(stats_t *st, char *line, const char *reset_ts)
{
    char *p = line;
    char *ts        = csv_next_field(&p);
    char *species   = csv_next_field(&p);
    csv_next_field(&p);                    /* confidence */
    csv_next_field(&p);                    /* frames */
    csv_next_field(&p);                    /* first_frame */
    char *corrected = csv_next_field(&p);
    char *latin     = csv_next_field(&p);

    if (!ts[0] || !species[0]) return;
    /* Stats are "since the last reset" (§3.4): skip rows older than the stored
     * reset epoch. ISO "YYYY-MM-DDT..." timestamps order lexicographically, so a
     * strcmp is a correct chronological compare. The visit log itself is never
     * deleted by a reset, so this filters counting only — labels/ROIs persist. */
    if (reset_ts && reset_ts[0] && strcmp(ts, reset_ts) < 0)
        return;
    /* User-confirmed label wins (§3.2/§3.4). Since v1.51 the relabel writes the
     * corrected species' own binomial into the latin column, so keep it (was
     * blanked when corrected labels had no known latin) — it localizes right. */
    if (corrected[0]) species = corrected;

    /* Confirmed false positive (classifier said "no bird" at/above the
     * threshold, §3.2): a motion trigger, not a bird visit. Kept out of the
     * bird buckets (species/daily/hourly/total) so wind events don't pollute
     * them, but tracked with its own count + first/last so the species table
     * can show it as an equal row (§3.4/v1.50). first/last mirror the species
     * rows' convention: first-encountered is kept, last-encountered updates. */
    if (strcmp(species, "no bird") == 0) {
        st->false_pos++;
        if (!st->fp_first[0]) strlcpy(st->fp_first, ts, sizeof(st->fp_first));
        strlcpy(st->fp_last, ts, sizeof(st->fp_last));
        return;
    }
    st->total++;

    /* Daily + hourly buckets need a synced timestamp ("YYYY-MM-DDTHH:...");
     * "unsynced" rows still count toward species totals. */
    if (strlen(ts) >= 13 && ts[4] == '-' && ts[10] == 'T') {
        char day[11];
        memcpy(day, ts, 10);
        day[10] = '\0';
        int i;
        for (i = 0; i < st->day_count; i++)
            if (strcmp(st->day[i], day) == 0) break;
        if (i == st->day_count && st->day_count < STATS_MAX_DAYS) {
            strlcpy(st->day[st->day_count], day, sizeof(st->day[0]));
            st->day_count++;
        } else if (i == st->day_count) {
            /* Full. The chart shows the NEWEST days, so a newer day evicts the
             * oldest bucket. Keeping the first 62 seen (before v3.44) froze
             * the chart once a card held more than 62 days of logs. */
            int old = 0;
            for (int k = 1; k < st->day_count; k++)
                if (strcmp(st->day[k], st->day[old]) < 0) old = k;
            if (strcmp(day, st->day[old]) > 0) {
                strlcpy(st->day[old], day, sizeof(st->day[0]));
                st->day_n[old] = 0;
                i = old;
            }
        }
        if (i < st->day_count) st->day_n[i]++;

        int hh = (ts[11] - '0') * 10 + (ts[12] - '0');
        if (hh >= 0 && hh < 24) st->hour[hh]++;
    }

    /* One species can be logged under different common names — Norwegian from
     * the relabel vocabulary, English from the cloud tier ("Bokfink" vs
     * "Common Chaffinch") — which used to split it into duplicate rows. The
     * Latin binomial is the species' identity whenever both sides have one
     * (v2.70); the raw name only decides for rows without Latin
     * ("unclassified", pre-v1.51 legacy rows). */
    int i;
    for (i = 0; i < st->sp_count; i++) {
        if (latin && latin[0] && st->sp_latin[i][0]) {
            if (strcmp(st->sp_latin[i], latin) == 0) break;
        } else if (strcmp(st->sp[i], species) == 0) {
            break;
        }
    }
    if (i == st->sp_count && st->sp_count < STATS_MAX_SPECIES) {
        strlcpy(st->sp[st->sp_count], species, sizeof(st->sp[0]));
        if (latin && latin[0])
            strlcpy(st->sp_latin[st->sp_count], latin, sizeof(st->sp_latin[0]));
        strlcpy(st->sp_first[st->sp_count], ts, sizeof(st->sp_first[0]));
        st->sp_count++;
    }
    if (i < st->sp_count) {
        st->sp_n[i]++;
        strlcpy(st->sp_last[i], ts, sizeof(st->sp_last[0]));
        if (latin && latin[0] && !st->sp_latin[i][0])
            strlcpy(st->sp_latin[i], latin, sizeof(st->sp_latin[0]));
    }
}

