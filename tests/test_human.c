/* open_memstream is POSIX.1-2008; glibc hides it under strict -std=c11
   unless asked. Must precede every include. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * test_human.c — golden-string tests for the human-output layout engine
 * (cli/human.c). Goldens are representative, real-shaped samples: most mirror
 * a current verb's output (vocabulary, row order, field formats), so a
 * renderer change trips them; a couple exercise the engine directly on a
 * shape no single verb emits (the error cluster — the error channel is a
 * one-line stderr diagnostic, not a block). A disagreement means either the
 * layout engine or a mirrored verb drifted.
 *
 * Uses open_memstream to capture FILE* output without touching the
 * filesystem.
 */
#include "test_harness.h"
#include "../cli/human.h"
#include "../cli/common.h"
#include "mos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *capture_block(const mos_cli_human_pair *pairs, size_t n, bool *ok)
{
    char *buf = NULL; size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    *ok = mos_cli_human_block(f, pairs, n);
    fclose(f);
    return buf;
}

static char *capture_table(const char *const *headers,
                           const char *const *cells,
                           size_t nrows, size_t ncols,
                           const bool *ra, bool *ok)
{
    char *buf = NULL; size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    *ok = mos_cli_human_table(f, headers, cells, nrows, ncols, ra);
    fclose(f);
    return buf;
}

static char *capture_table_ex(const char *const *headers,
                              const char *const *cells,
                              const char *const *display_cells,
                              size_t nrows, size_t ncols,
                              const bool *ra, bool *ok)
{
    char *buf = NULL; size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    *ok = mos_cli_human_table_ex(f, headers, cells, display_cells, nrows, ncols, ra);
    fclose(f);
    return buf;
}

TEST(human_block_ready_mounted_golden)
{
    /* A faithful `mos state` for a mounted, readable disc — an M-DISC data
       BD archive, which mounts on macOS as an ordinary volume (a UHD/BD video
       disc would NOT mount, so it would have no Volume row). Real emit order
       and vocabulary: separate Vendor/Product/Firmware rows (never one joined
       line), Volume is the name only, Media is "class — name". Pins the
       layout: keys right-aligned to the longest ("Registry ID"), ":  " gutter. */
    const mos_cli_human_pair pairs[] = {
        { "Registry ID", "4295032831" },
        { "BSD",         "/dev/disk4" },
        { "State",       "ready" },
        { "Media",       "BD — BD-R" },
        { "Volume",      "ARCHIVE" },
        { "Vendor",      "HL-DT-ST" },
        { "Product",     "BD-RE WH16NS60" },
        { "Firmware",    "1.00" },
    };
    bool ok; char *out = capture_block(pairs, 8, &ok);
    EXPECT_EQ(true, ok);
    EXPECT_STREQ(
        "Registry ID:  4295032831\n"
        "        BSD:  /dev/disk4\n"
        "      State:  ready\n"
        "      Media:  BD — BD-R\n"
        "     Volume:  ARCHIVE\n"
        "     Vendor:  HL-DT-ST\n"
        "    Product:  BD-RE WH16NS60\n"
        "   Firmware:  1.00\n", out);
    free(out);
    return 0;
}

TEST(human_block_empty_or_open_sense_and_dashes)
{
    /* A faithful `mos state` for an empty/open drive selected by BSD (so both
       Index and Registry ID show). Real emit order (state.c): Index, Registry
       ID, BSD, State, Sense, then separate Vendor/Product/Firmware rows — no
       media so no Media/Volume. Pins the Sense evidence row and the NULL→"-"
       dash on a structural row (BSD). */
    const mos_cli_human_pair pairs[] = {
        { "Index",       "1" },
        { "Registry ID", "4295032831" },
        { "BSD",         NULL },
        { "State",       "empty_or_open" },
        { "Sense",       "02/3a/01" },
        { "Vendor",      "HL-DT-ST" },
        { "Product",     "BD-RE WH16NS60" },
        { "Firmware",    "1.00" },
    };
    bool ok; char *out = capture_block(pairs, 8, &ok);
    EXPECT_EQ(true, ok);
    EXPECT_STREQ(
        "      Index:  1\n"
        "Registry ID:  4295032831\n"
        "        BSD:  -\n"
        "      State:  empty_or_open\n"
        "      Sense:  02/3a/01\n"
        "     Vendor:  HL-DT-ST\n"
        "    Product:  BD-RE WH16NS60\n"
        "   Firmware:  1.00\n", out);
    free(out);
    return 0;
}

TEST(human_block_error_evidence_cluster)
{
    /* Pure layout-engine case: no verb emits a human error block (the error
       channel is a one-line stderr diagnostic via mos_cli_emit_unknown_and_fail).
       This exercises the engine on a short-key cluster with a NULL tail row. */
    const mos_cli_human_pair pairs[] = {
        { "State", "error" },
        { "Stage", "probe" },
        { "Code",  "0xE00002C0  kIOReturnNotResponding" },
        { "Drive", NULL },
    };
    bool ok; char *out = capture_block(pairs, 4, &ok);
    EXPECT_EQ(true, ok);
    EXPECT_STREQ(
        "State:  error\n"
        "Stage:  probe\n"
        " Code:  0xE00002C0  kIOReturnNotResponding\n"
        "Drive:  -\n", out);
    free(out);
    return 0;
}

TEST(human_table_list_golden)
{
    /* The canonical 3-drive list table: Index right-aligned, NULL
       cells as "-", per-column widths from data, no trailing
       whitespace. */
    static const char *const headers[] =
        { "Index", "State", "Volume", "BSD", "Vendor", "Product", "Firmware" };
    static const char *const cells[] = {
        "1", "ready",         "/Volumes/ARRIVAL_4K", "/dev/disk4", "HL-DT-ST", "BD-RE WH16NS60", "1.00",
        "2", "empty_or_open", NULL,                  NULL,         "PIONEER",  "BD-RW BDR-XS07", "1.01",
        "3", "error",         NULL,                  "/dev/disk6", "ASUS",     "BW-16D1HT",      "3.10",
    };
    static const bool ra[] = { true, false, false, false, false, false, false };
    bool ok; char *out = capture_table(headers, cells, 3, 7, ra, &ok);
    EXPECT_EQ(true, ok);
    EXPECT_STREQ(
        " Index  State          Volume               BSD         Vendor    Product         Firmware\n"
        "     1  ready          /Volumes/ARRIVAL_4K  /dev/disk4  HL-DT-ST  BD-RE WH16NS60  1.00\n"
        "     2  empty_or_open  -                    -           PIONEER   BD-RW BDR-XS07  1.01\n"
        "     3  error          -                    /dev/disk6  ASUS      BW-16D1HT       3.10\n", out);
    free(out);
    return 0;
}

TEST(list_volume_cell_shows_path_only)
{
    char c[96];
    /* The list cell is the mount PATH only — the label stays in --json
       and on metadata's Volume row. */
    mos_cli_list_volume_cell("/Volumes/ARRIVAL", c, sizeof c);
    EXPECT_STREQ("/Volumes/ARRIVAL", c);
    /* Disambiguation rides on the path itself: macOS appends " 1" to the
       mount point, so the path alone carries what the label could not. */
    mos_cli_list_volume_cell("/Volumes/ARRIVAL 1", c, sizeof c);
    EXPECT_STREQ("/Volumes/ARRIVAL 1", c);
    /* Unmounted: nothing — caller renders this as "-". */
    mos_cli_list_volume_cell("", c, sizeof c);
    EXPECT_STREQ("", c);
    /* NULL is treated as empty (defensive; populate_row always fills). */
    mos_cli_list_volume_cell(NULL, c, sizeof c);
    EXPECT_STREQ("", c);
    return 0;
}

TEST(human_table_no_trailing_whitespace_any_line)
{
    static const char *const headers[] = { "Index", "State" };
    static const char *const cells[]   = { "1", "ready", "2", NULL };
    static const bool ra[] = { true, false };
    bool ok; char *out = capture_table(headers, cells, 2, 2, ra, &ok);
    EXPECT_EQ(true, ok);
    const char *p = out;
    while (*p) {
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        EXPECT_EQ(true, nl == p || nl[-1] != ' ');
        p = nl + 1;
    }
    free(out);
    return 0;
}

TEST(human_bsd_dev_node_contract)
{
    char buf[24];   /* in-domain max "/dev/disk4294967295" = 20 incl NUL */
    EXPECT_EQ(true,  mos_bsd_dev_node(4, buf, sizeof buf));
    EXPECT_STREQ("/dev/disk4", buf);
    EXPECT_EQ(false, mos_bsd_dev_node(-1, buf, sizeof buf));
    EXPECT_STREQ("", buf);
    /* DOMAIN pin with an ample buffer: a tiny buffer would pass via
       truncation and mask a missing domain guard. */
    EXPECT_EQ(false, mos_bsd_dev_node(123456789012LL, buf, sizeof buf));
    EXPECT_STREQ("", buf);
    EXPECT_EQ(true,  mos_bsd_dev_node((int64_t)4294967295LL, buf, sizeof buf));
    EXPECT_STREQ("/dev/disk4294967295", buf);
    /* TRUNCATION pin, separately: in-domain unit, cap too small. */
    char tiny[6];
    EXPECT_EQ(false, mos_bsd_dev_node(7, tiny, sizeof tiny));
    EXPECT_STREQ("", tiny);
    return 0;
}

TEST(human_bytes_scaling)
{
    char b[24];
    /* B tier (< 1000) keeps integer bytes. */
    EXPECT_STREQ("0 B",   mos_cli_human_bytes(0, b, sizeof b));
    EXPECT_STREQ("512 B", mos_cli_human_bytes(512, b, sizeof b));
    EXPECT_STREQ("999 B", mos_cli_human_bytes(999, b, sizeof b));
    /* Decimal (1000-based) tier boundaries, one decimal place. */
    EXPECT_STREQ("1.0 kB", mos_cli_human_bytes(1000, b, sizeof b));
    EXPECT_STREQ("2.0 kB", mos_cli_human_bytes(2048, b, sizeof b));
    EXPECT_STREQ("4.1 MB", mos_cli_human_bytes(4096000ULL, b, sizeof b));  /* 4096 kB buffer */
    /* A 4.7 GB DVD and a 25 GB BD render as marketed. */
    EXPECT_STREQ("4.7 GB",  mos_cli_human_bytes(4700372992ULL, b, sizeof b));
    EXPECT_STREQ("25.0 GB", mos_cli_human_bytes(25025314816ULL, b, sizeof b));
    /* NULL/zero-cap tolerated (returns buf unchanged). */
    EXPECT(mos_cli_human_bytes(1, NULL, 0) == NULL);
    return 0;
}

TEST(human_rate_scaling)
{
    char b[24];
    /* Below 1000 kB/s stays kB/s (integer); no B/s tier exists. */
    EXPECT_STREQ("150 kB/s", mos_cli_human_rate(150, b, sizeof b));
    EXPECT_STREQ("999 kB/s", mos_cli_human_rate(999, b, sizeof b));
    /* At/above 1000 → MB/s, one decimal. */
    EXPECT_STREQ("1.0 MB/s",  mos_cli_human_rate(1000, b, sizeof b));
    EXPECT_STREQ("10.6 MB/s", mos_cli_human_rate(10560, b, sizeof b));
    EXPECT_STREQ("8.3 MB/s",  mos_cli_human_rate(8310, b, sizeof b));
    return 0;
}

TEST(human_rate_x_multiples)
{
    char b[40];
    /* Headline is the medium-native multiple; absolute rate in parens.
       BD 1x = 4500 kB/s, DVD 1x = 1385, CD 1x = 153.6, HD DVD 1x = 4568. */
    EXPECT_STREQ("~16.0× BD (72.0 MB/s)",
                 mos_cli_human_rate_x(72000, "bd", b, sizeof b));
    EXPECT_STREQ("~16.0× DVD (22.2 MB/s)",
                 mos_cli_human_rate_x(22160, "dvd", b, sizeof b));
    EXPECT_STREQ("~48.0× CD (7.4 MB/s)",
                 mos_cli_human_rate_x(7373, "cd", b, sizeof b));
    EXPECT_STREQ("~1.0× HD DVD (4.6 MB/s)",
                 mos_cli_human_rate_x(4568, "hd_dvd", b, sizeof b));
    /* No/unknown class → absolute rate alone (no fabricated multiple). */
    EXPECT_STREQ("72.0 MB/s", mos_cli_human_rate_x(72000, NULL, b, sizeof b));
    EXPECT_STREQ("72.0 MB/s", mos_cli_human_rate_x(72000, "mo", b, sizeof b));
    /* Zero rate degrades too. */
    EXPECT_STREQ("0 kB/s", mos_cli_human_rate_x(0, "bd", b, sizeof b));
    return 0;
}

TEST(human_table_ex_display_cells_used_for_render)
{
    /* display_cells override is rendered instead of the plain cell, while the
       plain cell still drives the column width. An ANSI-coded value that is
       longer in bytes than the plain cell must not inflate the column. */
    static const char *const headers[] = { "State", "BSD" };
    static const char *const cells[]   = { "ready", "/dev/disk4" };
    /* display cell is longer in bytes (ANSI codes) but same display width. */
    static const char *const dcells[]  = { "\033[32mready\033[0m", NULL };
    static const bool ra[] = { false, false };
    bool ok;
    char *out = capture_table_ex(headers, cells, dcells, 1, 2, ra, &ok);
    EXPECT_EQ(true, ok);
    /* The plain cell "ready" (5 chars) drives width; ANSI codes don't inflate it.
       "BSD" header is 3 chars, "/dev/disk4" is 10 → BSD column is 10 wide. */
    EXPECT_STREQ(
        " State  BSD\n"
        " \033[32mready\033[0m  /dev/disk4\n", out);
    free(out);
    return 0;
}

TEST(human_table_ex_null_display_cells_matches_base)
{
    /* When display_cells is NULL, mos_cli_human_table_ex must produce
       exactly the same output as mos_cli_human_table. */
    static const char *const headers[] = { "Index", "State" };
    static const char *const cells[]   = { "1", "ready", "2", NULL };
    static const bool ra[] = { true, false };
    bool ok_base, ok_ex;
    char *base = capture_table(headers, cells, 2, 2, ra, &ok_base);
    char *ex   = capture_table_ex(headers, cells, NULL, 2, 2, ra, &ok_ex);
    EXPECT_EQ(true, ok_base);
    EXPECT_EQ(true, ok_ex);
    EXPECT_STREQ(base, ex);
    free(base); free(ex);
    return 0;
}

/* Label function contract tests. */
TEST(human_profile_label_cd_dvd_bd)
{
    EXPECT_STREQ("CD-ROM",    mos_cli_profile_label(0x0008));
    EXPECT_STREQ("CD-R",      mos_cli_profile_label(0x0009));
    EXPECT_STREQ("CD-RW",     mos_cli_profile_label(0x000A));
    EXPECT_STREQ("DVD-ROM",   mos_cli_profile_label(0x0010));
    EXPECT_STREQ("DVD+RW",    mos_cli_profile_label(0x001A));
    EXPECT_STREQ("DVD+R DL",  mos_cli_profile_label(0x002B));
    EXPECT_STREQ("BD-ROM",    mos_cli_profile_label(0x0040));
    EXPECT_STREQ("BD-R",      mos_cli_profile_label(0x0041));
    EXPECT_STREQ("BD-RE",     mos_cli_profile_label(0x0043));
    /* HD DVD not labelled (impossible on Mac). */
    EXPECT_EQ(NULL, mos_cli_profile_label(0x0050));
    EXPECT_EQ(NULL, mos_cli_profile_label(0x0051));
    /* Unknown code. */
    EXPECT_EQ(NULL, mos_cli_profile_label(0x00FF));
    return 0;
}

TEST(human_class_label_tokens)
{
    EXPECT_STREQ("CD",     mos_cli_class_label("cd"));
    EXPECT_STREQ("DVD",    mos_cli_class_label("dvd"));
    EXPECT_STREQ("BD",     mos_cli_class_label("bd"));
    EXPECT_STREQ("HD DVD", mos_cli_class_label("hd_dvd"));
    EXPECT_EQ(NULL,        mos_cli_class_label("mo"));
    EXPECT_EQ(NULL,        mos_cli_class_label(NULL));
    return 0;
}

TEST(human_media_type_label_tokens)
{
    EXPECT_STREQ("CD-ROM",  mos_cli_media_type_label("cd_rom"));
    EXPECT_STREQ("BD-R",    mos_cli_media_type_label("bd_r"));
    EXPECT_STREQ("BD-RE",   mos_cli_media_type_label("bd_re"));
    EXPECT_STREQ("DVD-ROM", mos_cli_media_type_label("dvd_rom"));
    EXPECT_EQ(NULL,         mos_cli_media_type_label("unknown_type"));
    EXPECT_EQ(NULL,         mos_cli_media_type_label(NULL));
    return 0;
}

void register_human_tests(void)
{
    RUN(human_block_ready_mounted_golden);
    RUN(human_block_empty_or_open_sense_and_dashes);
    RUN(human_block_error_evidence_cluster);
    RUN(human_table_list_golden);
    RUN(list_volume_cell_shows_path_only);
    RUN(human_table_no_trailing_whitespace_any_line);
    RUN(human_bsd_dev_node_contract);
    RUN(human_bytes_scaling);
    RUN(human_rate_scaling);
    RUN(human_rate_x_multiples);
    RUN(human_table_ex_display_cells_used_for_render);
    RUN(human_table_ex_null_display_cells_matches_base);
    RUN(human_profile_label_cd_dvd_bd);
    RUN(human_class_label_tokens);
    RUN(human_media_type_label_tokens);
}
