/*
 * cli/color.h — terminal color and geometry helpers for the CLI's human views.
 *
 * Static inline; no companion .c file. Color is gated on isatty(STDOUT_FILENO)
 * + NO_COLOR (https://no-color.org). Truncation is gated on isatty only
 * (independent of NO_COLOR — the terminal width is real even if the user
 * prefers no color). macOS / POSIX only.
 */
#ifndef MOS_CLI_COLOR_H
#define MOS_CLI_COLOR_H

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* Color mode, set once by the CLI's option parser (--color / --no-color) and
   read by mos_cli_color_enabled() below. AUTO is the default and the only mode
   that consults the terminal + environment; ALWAYS/NEVER are explicit
   overrides (the `git`/`ls`/`ripgrep` convention) so a user can force color
   through a pipe (`mos list --color=always | less -R`) or off in CI.
   Defined in cli/common.c. */
enum {
    MOS_CLI_COLOR_AUTO = 0,   /* tty + NO_COLOR/TERM=dumb checks (default) */
    MOS_CLI_COLOR_ALWAYS,     /* force on regardless of tty/env */
    MOS_CLI_COLOR_NEVER       /* force off regardless of tty/env */
};
extern int mos_cli_color_mode;

/* True iff color should be emitted. AUTO (the default) requires stdout to be a
   terminal with NO_COLOR unset and TERM not "dumb" (https://no-color.org and
   the de-facto dumb-terminal convention); ALWAYS/NEVER short-circuit it. */
static inline bool mos_cli_color_enabled(void)
{
    if (mos_cli_color_mode == MOS_CLI_COLOR_ALWAYS) return true;
    if (mos_cli_color_mode == MOS_CLI_COLOR_NEVER)  return false;
    if (!isatty(STDOUT_FILENO) || getenv("NO_COLOR")) return false;
    const char *term = getenv("TERM");
    return !(term && strcmp(term, "dumb") == 0);
}

/* Terminal column count. Returns 0 when stdout is not a terminal so callers
   can use 0 as "no truncation needed". Falls back to 80 when the tty ioctl
   fails (e.g. a terminal that does not support TIOCGWINSZ). */
static inline int mos_cli_term_cols(void)
{
    if (!isatty(STDOUT_FILENO)) return 0;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col;
    return 80;
}

/* ANSI SGR open sequence for a given state string, or "" when colors are off
   or the state is unrecognized (unknown/unclassified — no hue assigned).
   Palette: ready → green; error/device_fault/media_unreadable → red;
   busy/loading/formatting → yellow; empty/open/empty_or_open → dim. */
static inline const char *mos_cli_state_open(const char *state)
{
    if (!mos_cli_color_enabled() || !state) return "";
    if (strcmp(state, "ready") == 0)                  return "\033[32m";
    if (strcmp(state, "error")          == 0 ||
        strcmp(state, "device_fault")   == 0 ||
        strcmp(state, "media_unreadable") == 0)       return "\033[31m";
    if (strcmp(state, "busy")       == 0 ||
        strcmp(state, "loading")    == 0 ||
        strcmp(state, "formatting") == 0)             return "\033[33m";
    if (strcmp(state, "empty")        == 0 ||
        strcmp(state, "open")         == 0 ||
        strcmp(state, "empty_or_open") == 0)          return "\033[2m";
    return "";
}

/* Bullet glyph for the list table's leading indicator column.
 *
 * Two forms are needed because the layout engine (mos_cli_human_table_ex)
 * computes column widths from the PLAIN cell strings and uses display strings
 * only for actual rendering:
 *
 *   plain:   always "*" — 1 byte, 1 display column.  Gives the engine the
 *            correct column width regardless of the glyph's byte length.
 *
 *   display: a colored UTF-8 glyph — 3 bytes for the character, plus ANSI SGR
 *            open/close sequences.  Visually 1 column wide on every macOS
 *            terminal; byte length does not match display width, which is why
 *            the plain form drives the layout.
 */
static inline const char *mos_cli_state_bullet_plain(void)
{
    return "*";
}

static inline const char *mos_cli_state_bullet_display(const char *state)
{
    if (!state) return "\033[2m?\033[0m";
    if (strcmp(state, "ready") == 0)                  return "\033[32m\xe2\x97\x8f\033[0m"; /* ● */
    if (strcmp(state, "error")          == 0 ||
        strcmp(state, "device_fault")   == 0 ||
        strcmp(state, "media_unreadable") == 0)       return "\033[31m\xe2\x9c\x95\033[0m"; /* ✕ */
    if (strcmp(state, "busy")       == 0 ||
        strcmp(state, "loading")    == 0 ||
        strcmp(state, "formatting") == 0)             return "\033[33m\xe2\x97\x90\033[0m"; /* ◐ */
    if (strcmp(state, "empty")        == 0 ||
        strcmp(state, "open")         == 0 ||
        strcmp(state, "empty_or_open") == 0)          return "\033[2m\xe2\x97\x8b\033[0m";  /* ○ */
    return "\033[2m?\033[0m";
}

#endif /* MOS_CLI_COLOR_H */
