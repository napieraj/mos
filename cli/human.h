/*
 * cli/human.h — layout engine for the human-readable CLI views. Writes to
 * a FILE*, knows no result struct; the caller owns vocabulary and
 * suppression (a suppressed pair is simply absent).
 *
 * SANITIZATION CONTRACT: keys/values/cells print verbatim (fputs), so any
 * drive-controlled bytes (identity strings) must arrive pre-escaped via
 * mos_safe_ascii — done in cli/state.c emit_human and cli/common.c
 * query_row. Library vocabulary is printable ASCII by construction.
 */
#ifndef MOS_CLI_HUMAN_H
#define MOS_CLI_HUMAN_H

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Human-readable unit scaling for the CLI's text views ONLY — the JSON
   emitters keep the raw integers (kbps, byte counts) for machine parsing.
   Decimal (1000-based) throughout: GET PERFORMANCE reports decimal kB/s and
   disc capacities are marketed decimal (a "4.7 GB" DVD). One decimal place.
   Both write into `buf` (cap bytes) and return it for inline use. */

/* Byte count → "N B" / "N.N kB" / "N.N MB" / "N.N GB". The B tier keeps
   integer bytes (block sizes like 2048 B); larger tiers scale with one
   decimal. */
const char *mos_cli_human_bytes(uint64_t bytes, char *buf, size_t cap);

/* Transfer rate in kB/s → "N kB/s" (below 1000) or "N.N MB/s". Optical
   rates never fall below CD 1x (~150 kB/s), so there is no B/s tier. */
const char *mos_cli_human_rate(uint32_t kbps, char *buf, size_t cap);

/* Transfer rate scaled to the loaded medium's native 1x multiple, the
   headline figure with the absolute rate in parentheses:
   "~3.6× BD (16.0 MB/s)". `media_class` is mos_profile_class() of the loaded
   disc ("cd"/"dvd"/"bd"/"hd_dvd"); when it is NULL or a class with no defined
   1x base (no medium / MO / unknown), this degrades to a plain
   mos_cli_human_rate (just the absolute rate). The multiple is approximate
   (nominal 1x bases), hence the "~". */
const char *mos_cli_human_rate_x(uint32_t kbps, const char *media_class,
                                 char *buf, size_t cap);

typedef struct mos_cli_human_pair {
    const char *key;   /* never NULL */
    const char *val;   /* NULL renders as "-" */
} mos_cli_human_pair;

/* Aligned key block. Keys right-aligned to the longest key in `pairs`;
   "key:  value" with a two-space gutter. Returns false only on
   invalid arguments (NULL f/pairs, n == 0). */
bool mos_cli_human_block(FILE *f, const mos_cli_human_pair *pairs, size_t n);

/* Column-aligned table. `cells` is row-major, nrows x ncols; headers
   has ncols entries; right_align (may be NULL = all left) has ncols
   flags. NULL cells render "-". One leading space before the first
   column (matches the design mocks); two-space gutters; no trailing
   whitespace on any line. */
bool mos_cli_human_table(FILE *f,
                     const char *const *headers,
                     const char *const *cells,
                     size_t nrows, size_t ncols,
                     const bool *right_align);

#endif /* MOS_CLI_HUMAN_H */
