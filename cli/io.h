/*
 * cli/io.h — shared stdout writers for the mos CLI. The escaping
 * rules live once in the library (mos_json_escape / mos_safe_ascii,
 * src/mos_strings.c); these wrap them for FILE output so every
 * command emits identically.
 */
#ifndef MOS_CLI_IO_H
#define MOS_CLI_IO_H

#include <stdio.h>
#include <stdint.h>

/* Write `s` to `f` as a complete JSON string value — double-quoted and
 * escaped — or the bare literal `null` when s == NULL. The quotes are
 * part of the output; callers must NOT add their own. */
void mos_cli_json_str(FILE *f, const char *s);

/* Write the whole-disk unit to `f` as a complete JSON string value:
 * the device node "/dev/diskN" for a unit in [0, UINT32_MAX] (the form
 * all four schemas pin), or the literal `null` for unit < 0 (no media)
 * or a domain-refused unit. Quotes are part of the output. */
void mos_cli_bsd_dev_node(FILE *f, int64_t unit);

/* Write `s` to `f` with every non-printable byte rendered as \xNN via
 * mos_safe_ascii (defends a tty against control-sequence injection from
 * device-controlled strings). No surrounding quotes. NULL is a no-op.
 * Renders through a fixed 4 KiB buffer and silently drops anything
 * beyond it (worst case ~1 KiB of input if every byte escapes) —
 * unlike mos_cli_json_str's measure-then-allocate two-pass. Fine for
 * every current call site (argv, env values, SPC identity strings,
 * all far below the floor); not for unbounded payloads. \xNN
 * truncation cannot split into something dangerous the way JSON
 * mid-escape truncation can. */
void mos_cli_safe_ascii(FILE *f, const char *s);

/* Outcome of finalizing stdout after a tool has written its output:
 *   MOS_CLI_STDOUT_OK           write succeeded
 *   MOS_CLI_STDOUT_PIPE_CLOSED  EPIPE — downstream consumer closed
 *                               (e.g. `mos --json | head`); producer-
 *                               clean, callers map to EX_OK.
 *   MOS_CLI_STDOUT_WRITE_ERROR  any other failure; callers map to
 *                               EX_IOERR. */
typedef enum {
    MOS_CLI_STDOUT_OK = 0,
    MOS_CLI_STDOUT_PIPE_CLOSED,
    MOS_CLI_STDOUT_WRITE_ERROR
} mos_cli_stdout_status;

/* fflush(stdout) + sticky-ferror check, classified per the enum above.
 * Call once after a command has written all its output. The ferror /
 * errno reasoning lives at the implementation (cli/io.c). */
mos_cli_stdout_status mos_cli_stdout_finalize(void);

#endif /* MOS_CLI_IO_H */
