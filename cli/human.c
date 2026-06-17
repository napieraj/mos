/*
 * cli/human.c — see human.h. Layout only; vocabulary and suppression are
 * the caller's. The exact bytes (alignment, two-space gutters, NULL -> "-",
 * right-aligned columns and headers) are pinned by the golden-string tests
 * in tests/test_human.c.
 */
#include "human.h"

#include <string.h>

#define MOS_CLI_HUMAN_DASH "-"

static const char *cell_or_dash(const char *s)
{
    return s ? s : MOS_CLI_HUMAN_DASH;
}

const char *mos_cli_human_bytes(uint64_t bytes, char *buf, size_t cap)
{
    if (!buf || cap == 0) return buf;
    if (bytes < 1000ULL)
        snprintf(buf, cap, "%llu B", (unsigned long long)bytes);
    else if (bytes < 1000ULL * 1000ULL)
        snprintf(buf, cap, "%.1f kB", (double)bytes / 1e3);
    else if (bytes < 1000ULL * 1000ULL * 1000ULL)
        snprintf(buf, cap, "%.1f MB", (double)bytes / 1e6);
    else
        snprintf(buf, cap, "%.1f GB", (double)bytes / 1e9);
    return buf;
}

const char *mos_cli_human_rate(uint32_t kbps, char *buf, size_t cap)
{
    if (!buf || cap == 0) return buf;
    if (kbps < 1000u)
        snprintf(buf, cap, "%u kB/s", kbps);
    else
        snprintf(buf, cap, "%.1f MB/s", (double)kbps / 1e3);
    return buf;
}

/* Nominal 1x data rate (decimal kB/s) per optical media class, and the
   display label. Bases: CD 1x = 75 sectors/s x 2048 B = 153.6 kB/s; DVD 1x =
   11.08 Mbit/s = 1385 kB/s; BD 1x = 36 Mbit/s = 4500 kB/s; HD DVD 1x =
   36.55 Mbit/s = 4568 kB/s (SPEC.md). Returns 0 base for a class with no
   defined multiple (caller then shows the absolute rate alone). */
static double rate_base_kbps(const char *media_class, const char **label)
{
    if (media_class) {
        if (strcmp(media_class, "cd")     == 0) { *label = "CD";     return 153.6; }
        if (strcmp(media_class, "dvd")    == 0) { *label = "DVD";    return 1385.0; }
        if (strcmp(media_class, "bd")     == 0) { *label = "BD";     return 4500.0; }
        if (strcmp(media_class, "hd_dvd") == 0) { *label = "HD DVD"; return 4568.0; }
    }
    *label = NULL;
    return 0.0;
}

const char *mos_cli_human_rate_x(uint32_t kbps, const char *media_class,
                                 char *buf, size_t cap)
{
    if (!buf || cap == 0) return buf;
    const char *label = NULL;
    double base = rate_base_kbps(media_class, &label);
    if (base <= 0.0 || kbps == 0u)            /* no base (or no rate): absolute only */
        return mos_cli_human_rate(kbps, buf, cap);

    char abs[24];
    (void)mos_cli_human_rate(kbps, abs, sizeof abs);
    snprintf(buf, cap, "~%.1f× %s (%s)", (double)kbps / base, label, abs);
    return buf;
}

bool mos_cli_human_block(FILE *f, const mos_cli_human_pair *pairs, size_t n)
{
    if (!f || !pairs || n == 0) return false;

    size_t keyw = 0;
    for (size_t i = 0; i < n; i++) {
        if (!pairs[i].key) return false;
        size_t l = strlen(pairs[i].key);
        if (l > keyw) keyw = l;
    }

    for (size_t i = 0; i < n; i++) {
        size_t l   = strlen(pairs[i].key);
        size_t pad = keyw - l;
        for (size_t s = 0; s < pad; s++) fputc(' ', f);
        fputs(pairs[i].key, f);
        fputs(":  ", f);
        fputs(cell_or_dash(pairs[i].val), f);
        fputc('\n', f);
    }
    return true;
}

bool mos_cli_human_table(FILE *f,
                     const char *const *headers,
                     const char *const *cells,
                     size_t nrows, size_t ncols,
                     const bool *right_align)
{
    if (!f || !headers || ncols == 0) return false;
    if (nrows > 0 && !cells) return false;

    /* Column widths: max of header and every cell in the column. */
    size_t w[32];
    if (ncols > 32) return false;
    for (size_t c = 0; c < ncols; c++) {
        if (!headers[c]) return false;
        w[c] = strlen(headers[c]);
        for (size_t r = 0; r < nrows; r++) {
            size_t l = strlen(cell_or_dash(cells[r * ncols + c]));
            if (l > w[c]) w[c] = l;
        }
    }

    /* Leading space, cells padded to w[c], two-space gutters, no trailing
       whitespace (last left-aligned column unpadded). Pinned by the
       golden-string tests. */
    for (size_t r = 0; r <= nrows; r++) {
        const bool header_row = (r == 0);
        fputc(' ', f);
        for (size_t c = 0; c < ncols; c++) {
            const char *s = header_row ? headers[c]
                                       : cell_or_dash(cells[(r - 1) * ncols + c]);
            size_t l   = strlen(s);
            size_t pad = w[c] - l;
            bool   ra  = right_align && right_align[c] && !header_row;
            /* A right-aligned column gets a right-aligned header too, so
               "Index" lines up over its numerals. */
            if (right_align && right_align[c] && header_row) ra = true;
            if (ra) {
                for (size_t s2 = 0; s2 < pad; s2++) fputc(' ', f);
                fputs(s, f);
            } else {
                fputs(s, f);
                if (c + 1 < ncols)
                    for (size_t s2 = 0; s2 < pad; s2++) fputc(' ', f);
            }
            if (c + 1 < ncols) fputs("  ", f);
        }
        fputc('\n', f);
    }
    return true;
}
