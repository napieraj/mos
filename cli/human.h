/*
 * cli/human.h — layout engine for the human-readable CLI views.
 *
 * Pure: writes to a FILE* (tests use open_memstream), no I/O of its
 * own, no knowledge of result structs. The CLI assembles (key, value)
 * pairs / table cells from accessor values; THIS layer owns alignment,
 * gutters, and width computation, so the golden-string tests in
 * tests/test_human.c pin the exact bytes of the design mocks
 * (doc/research/2026-06-10-cli-design.md).
 *
 * Conventions:
 *   - Aligned key blocks: keys right-aligned to the longest key,
 *     ":" then two-space gutter, one pair per line.
 *   - Suppression is the CALLER's job: a pair the schema suppresses is
 *     simply not in the array. A pair with val == NULL renders "-"
 *     (structural rows — addressing/identity — show absence, they do
 *     not vanish).
 *   - Tables: per-column width = max(header, widest cell); columns
 *     separated by two spaces; right_align flags per column (Index);
 *     NULL cells render "-".
 *   - All strings are emitted verbatim — vocabulary, including enum
 *     values, is the caller's contract with the schemas.
 *
 * SANITIZATION CONTRACT: this is a LAYOUT engine — keys, values, and
 * cells are printed verbatim (fputs). Any drive-controlled bytes
 * (identity strings) must arrive pre-escaped; the sanitization sites
 * are cli/status.c emit_human and cli/common.c query_row, both via
 * mos_safe_ascii. Library-controlled vocabulary (state names, profile
 * names, formatted units/ids) is printable ASCII by construction.
 */
#ifndef MOS_CLI_HUMAN_H
#define MOS_CLI_HUMAN_H

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>

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
