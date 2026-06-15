/*
 * cli/human.h — layout engine for the human-readable CLI views. Pure:
 * writes to a FILE*, no result-struct knowledge; the caller owns
 * vocabulary and suppression (a suppressed pair is simply absent).
 *
 * SANITIZATION CONTRACT: keys/values/cells print verbatim (fputs), so
 * any drive-controlled bytes (identity strings) must arrive pre-escaped.
 * Sanitization sites are cli/status.c emit_human and cli/common.c
 * query_row, both via mos_safe_ascii; library vocabulary is printable
 * ASCII by construction.
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
