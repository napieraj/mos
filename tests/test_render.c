/*
 * test_render.c — hostile-input pinning for mos_json_escape and
 * mos_safe_ascii.
 *
 * These functions render drive-controlled bytes (INQUIRY vendor and
 * product strings, sense fields surfaced as part of error contexts)
 * to JSON and to plain tty output. The fixtures here cover:
 *
 *   - Bounds: NULL input, empty input, single-character.
 *   - JSON-mandatory escape forms (RFC 8259): ", \, \b, \f, \n, \r, \t.
 *   - Control bytes < 0x20 not covered by named forms (\u00XX).
 *   - 0x7F (DEL) escaped to defend against terminal-aware consumers.
 *   - High bytes 0x80-0xFF escaped to keep JSON encoding-portable.
 *   - Realistic terminal-injection payloads:
 *       * ANSI color sequence (ESC [ 31 m red ESC [ 0 m)
 *       * OSC 52 clipboard write attempt (ESC ] 52 ; c ; <BASE64> BEL)
 *       * Title-bar manipulation (ESC ] 0 ; <title> BEL)
 *       * Cursor-position report request (ESC [ 6 n)
 *   - Truncation: returns required length on a too-small buffer,
 *     guarantees NUL termination when out_cap >= 1.
 *   - Zero-capacity buffer: no write, accurate length still returned.
 *
 * No I/O, no allocation, no globals. Each fixture is a pure
 * input→output assertion.
 */

#include "test_harness.h"
#include "mos.h"
#include <string.h>
#include <stddef.h>

/* ---- mos_json_escape ------------------------------------------------- */

TEST(json_null_in_yields_empty_output)
{
    char out[16] = "untouched";
    size_t n = mos_json_escape(NULL, out, sizeof(out));
    EXPECT_EQ((size_t)0, n);
    EXPECT(strcmp(out, "") == 0);
    return 0;
}

TEST(json_empty_in_yields_empty_output)
{
    char out[16];
    size_t n = mos_json_escape("", out, sizeof(out));
    EXPECT_EQ((size_t)0, n);
    EXPECT(strcmp(out, "") == 0);
    return 0;
}

TEST(json_plain_text_passes_through)
{
    char out[64];
    size_t n = mos_json_escape("hello world", out, sizeof(out));
    EXPECT_EQ((size_t)11, n);
    EXPECT(strcmp(out, "hello world") == 0);
    return 0;
}

TEST(json_escapes_quote)
{
    char out[32];
    mos_json_escape("a\"b", out, sizeof(out));
    EXPECT(strcmp(out, "a\\\"b") == 0);
    return 0;
}

TEST(json_escapes_backslash)
{
    char out[32];
    mos_json_escape("a\\b", out, sizeof(out));
    EXPECT(strcmp(out, "a\\\\b") == 0);
    return 0;
}

TEST(json_named_escapes_for_control_bytes)
{
    char out[64];
    mos_json_escape("a\bb\fc\nd\re\tf", out, sizeof(out));
    EXPECT(strcmp(out, "a\\bb\\fc\\nd\\re\\tf") == 0);
    return 0;
}

TEST(json_escapes_unnamed_control_bytes_as_u00XX)
{
    /* 0x01 SOH — no named form. */
    char in[]  = "a\x01" "b";
    char out[32];
    mos_json_escape(in, out, sizeof(out));
    EXPECT(strcmp(out, "a\\u0001b") == 0);
    return 0;
}

TEST(json_escapes_NUL_via_strlen_boundary)
{
    /* C strings end at NUL; mos_json_escape is strlen-bound. A NUL
       in the middle of a buffer is just end-of-string from its
       perspective. Pinning this behavior so callers don't expect
       embedded-NUL handling. */
    char in[] = "ab";  /* 'a' 'b' '\0' */
    char out[16];
    size_t n = mos_json_escape(in, out, sizeof(out));
    EXPECT_EQ((size_t)2, n);
    EXPECT(strcmp(out, "ab") == 0);
    return 0;
}

TEST(json_escapes_DEL_0x7F)
{
    /* 0x7F (DEL) is escaped defensively. RFC 8259 doesn't mandate
       this but terminal-aware downstream consumers can mishandle
       raw DEL bytes. */
    char in[]  = "a\x7f" "b";
    char out[32];
    mos_json_escape(in, out, sizeof(out));
    EXPECT(strcmp(out, "a\\u007fb") == 0);
    return 0;
}

TEST(json_escapes_high_bytes_as_u00XX)
{
    /* 0x80-0xFF: INQUIRY isn't guaranteed UTF-8; escape so the JSON
       stays valid regardless of consumer encoding. */
    char in[]  = "a\xc3" "b\xff" "c";
    char out[64];
    mos_json_escape(in, out, sizeof(out));
    EXPECT(strcmp(out, "a\\u00c3b\\u00ffc") == 0);
    return 0;
}

TEST(json_escapes_ansi_color_sequence)
{
    /* ESC [ 31 m red ESC [ 0 m — what a hostile drive's vendor
       string could contain to color-bomb a tty if not escaped. */
    char in[]  = "\x1b[31mred\x1b[0m";
    char out[64];
    mos_json_escape(in, out, sizeof(out));
    EXPECT(strcmp(out, "\\u001b[31mred\\u001b[0m") == 0);
    return 0;
}

TEST(json_escapes_osc52_clipboard_payload)
{
    /* OSC 52 ; c ; PAYLOAD BEL — terminal-mediated clipboard write.
       The most dangerous escape in this fixture set; never let a
       drive emit one. */
    char in[]  = "\x1b]52;c;cm9vdGtpdA==\x07";
    char out[128];
    mos_json_escape(in, out, sizeof(out));
    EXPECT(strcmp(out, "\\u001b]52;c;cm9vdGtpdA==\\u0007") == 0);
    return 0;
}

TEST(json_escapes_title_bar_manipulation)
{
    /* ESC ] 0 ; <title> BEL — terminal title bar overwrite. */
    char in[]  = "\x1b]0;PWNED\x07";
    char out[64];
    mos_json_escape(in, out, sizeof(out));
    EXPECT(strcmp(out, "\\u001b]0;PWNED\\u0007") == 0);
    return 0;
}

TEST(json_truncation_returns_required_length)
{
    /* Buffer too small; verify truncation behavior:
       1. Return value is the full required length.
       2. Output is still NUL-terminated.
       3. Output is a prefix of what would have been written. */
    char out[8];
    size_t n = mos_json_escape("hello\"world", out, sizeof(out));
    EXPECT_EQ((size_t)12, n);  /* h e l l o \" w o r l d = 12 bytes */
    EXPECT_EQ((size_t)7, strlen(out));  /* 7 chars + NUL = 8 */
    EXPECT(strncmp(out, "hello\\\"", 7) == 0);
    return 0;
}

TEST(json_zero_capacity_no_write_accurate_length)
{
    /* out_cap == 0: no NUL termination, no write, but the return
       value still tells the caller how big a buffer would suffice. */
    char out[16];
    memset(out, '!', sizeof(out));
    size_t n = mos_json_escape("hello", out, 0);
    EXPECT_EQ((size_t)5, n);
    /* Verify nothing was written. */
    for (size_t i = 0; i < sizeof(out); i++) {
        EXPECT_EQ((int)'!', (int)out[i]);
    }
    return 0;
}

TEST(json_capacity_one_yields_just_NUL)
{
    /* out_cap == 1: room only for the NUL terminator. */
    char out[1] = { '?' };
    size_t n = mos_json_escape("abc", out, 1);
    EXPECT_EQ((size_t)3, n);
    EXPECT_EQ((int)'\0', (int)out[0]);
    return 0;
}

TEST(json_realistic_inquiry_payload)
{
    /* Plausible-looking real INQUIRY: vendor='HL-DT-ST',
       product='BD-RE BH16NS55'. No escapes expected. */
    char out[64];
    mos_json_escape("HL-DT-ST BD-RE BH16NS55", out, sizeof(out));
    EXPECT(strcmp(out, "HL-DT-ST BD-RE BH16NS55") == 0);
    return 0;
}

/* ---- mos_safe_ascii -------------------------------------------------- */

TEST(safe_null_in_yields_empty_output)
{
    char out[16] = "untouched";
    size_t n = mos_safe_ascii(NULL, out, sizeof(out));
    EXPECT_EQ((size_t)0, n);
    EXPECT(strcmp(out, "") == 0);
    return 0;
}

TEST(safe_empty_in_yields_empty_output)
{
    char out[16];
    size_t n = mos_safe_ascii("", out, sizeof(out));
    EXPECT_EQ((size_t)0, n);
    EXPECT(strcmp(out, "") == 0);
    return 0;
}

TEST(safe_plain_text_passes_through)
{
    char out[64];
    size_t n = mos_safe_ascii("hello world", out, sizeof(out));
    EXPECT_EQ((size_t)11, n);
    EXPECT(strcmp(out, "hello world") == 0);
    return 0;
}

TEST(safe_escapes_control_bytes_as_xNN)
{
    char in[]  = "a\x01\x07\x1f" "b";
    char out[32];
    mos_safe_ascii(in, out, sizeof(out));
    EXPECT(strcmp(out, "a\\x01\\x07\\x1fb") == 0);
    return 0;
}

TEST(safe_escapes_DEL)
{
    char in[]  = "a\x7f" "b";
    char out[32];
    mos_safe_ascii(in, out, sizeof(out));
    EXPECT(strcmp(out, "a\\x7fb") == 0);
    return 0;
}

TEST(safe_escapes_high_bytes)
{
    char in[]  = "a\xc3" "b\xff" "c";
    char out[32];
    mos_safe_ascii(in, out, sizeof(out));
    EXPECT(strcmp(out, "a\\xc3b\\xffc") == 0);
    return 0;
}

TEST(safe_escapes_ansi_color_sequence)
{
    char in[]  = "\x1b[31mred\x1b[0m";
    char out[64];
    mos_safe_ascii(in, out, sizeof(out));
    EXPECT(strcmp(out, "\\x1b[31mred\\x1b[0m") == 0);
    return 0;
}

TEST(safe_escapes_osc52_clipboard_payload)
{
    char in[]  = "\x1b]52;c;cm9vdGtpdA==\x07";
    char out[128];
    mos_safe_ascii(in, out, sizeof(out));
    EXPECT(strcmp(out, "\\x1b]52;c;cm9vdGtpdA==\\x07") == 0);
    return 0;
}

TEST(safe_escapes_title_bar_manipulation)
{
    char in[]  = "\x1b]0;PWNED\x07";
    char out[64];
    mos_safe_ascii(in, out, sizeof(out));
    EXPECT(strcmp(out, "\\x1b]0;PWNED\\x07") == 0);
    return 0;
}

TEST(safe_escapes_cursor_position_report_request)
{
    /* ESC [ 6 n — terminal responds with current cursor coords.
       Some terminal stacks chain into command-line injection if
       the response is interpreted. */
    char in[]  = "\x1b[6n";
    char out[32];
    mos_safe_ascii(in, out, sizeof(out));
    EXPECT(strcmp(out, "\\x1b[6n") == 0);
    return 0;
}

TEST(safe_truncation_returns_required_length)
{
    char out[8];
    size_t n = mos_safe_ascii("hello\x1bworld", out, sizeof(out));
    /* h e l l o \x1b w o r l d → 5 + 4 + 5 = 14 chars when escaped. */
    EXPECT_EQ((size_t)14, n);
    EXPECT_EQ((size_t)7, strlen(out));
    EXPECT(strncmp(out, "hello\\x", 7) == 0);
    return 0;
}

TEST(safe_zero_capacity_no_write_accurate_length)
{
    char out[16];
    memset(out, '!', sizeof(out));
    size_t n = mos_safe_ascii("hello", out, 0);
    EXPECT_EQ((size_t)5, n);
    for (size_t i = 0; i < sizeof(out); i++) {
        EXPECT_EQ((int)'!', (int)out[i]);
    }
    return 0;
}

TEST(safe_realistic_inquiry_payload)
{
    char out[64];
    mos_safe_ascii("HL-DT-ST BD-RE BH16NS55", out, sizeof(out));
    EXPECT(strcmp(out, "HL-DT-ST BD-RE BH16NS55") == 0);
    return 0;
}

/* ---- Registration ---------------------------------------------------- */

void register_render_tests(void);
void register_render_tests(void)
{
    /* mos_json_escape */
    RUN(json_null_in_yields_empty_output);
    RUN(json_empty_in_yields_empty_output);
    RUN(json_plain_text_passes_through);
    RUN(json_escapes_quote);
    RUN(json_escapes_backslash);
    RUN(json_named_escapes_for_control_bytes);
    RUN(json_escapes_unnamed_control_bytes_as_u00XX);
    RUN(json_escapes_NUL_via_strlen_boundary);
    RUN(json_escapes_DEL_0x7F);
    RUN(json_escapes_high_bytes_as_u00XX);
    RUN(json_escapes_ansi_color_sequence);
    RUN(json_escapes_osc52_clipboard_payload);
    RUN(json_escapes_title_bar_manipulation);
    RUN(json_truncation_returns_required_length);
    RUN(json_zero_capacity_no_write_accurate_length);
    RUN(json_capacity_one_yields_just_NUL);
    RUN(json_realistic_inquiry_payload);

    /* mos_safe_ascii */
    RUN(safe_null_in_yields_empty_output);
    RUN(safe_empty_in_yields_empty_output);
    RUN(safe_plain_text_passes_through);
    RUN(safe_escapes_control_bytes_as_xNN);
    RUN(safe_escapes_DEL);
    RUN(safe_escapes_high_bytes);
    RUN(safe_escapes_ansi_color_sequence);
    RUN(safe_escapes_osc52_clipboard_payload);
    RUN(safe_escapes_title_bar_manipulation);
    RUN(safe_escapes_cursor_position_report_request);
    RUN(safe_truncation_returns_required_length);
    RUN(safe_zero_capacity_no_write_accurate_length);
    RUN(safe_realistic_inquiry_payload);
}
