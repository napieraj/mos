/*
 * cli/io.h — shared stdout writers for the mos CLI. JSON escaping and
 * ASCII sanitizing live once in the library (src/mos_strings.c); these
 * wrap them for FILE output so every command emits identically.
 */
#ifndef MOS_CLI_IO_H
#define MOS_CLI_IO_H

#include <stdio.h>
#include <stdint.h>

/* Write `s` as a complete double-quoted, escaped JSON string value, or
 * the literal `null` when s == NULL. The quotes are part of the output;
 * callers must not add their own. */
void mos_cli_json_str(FILE *f, const char *s);

/* Write the whole-disk unit as a JSON string value: the device node
 * "/dev/diskN" for an in-domain unit, or `null` for unit < 0 (no media)
 * or a refused unit. Quotes are part of the output. */
void mos_cli_bsd_dev_node(FILE *f, int64_t unit);

/* Write `s` with every non-printable byte rendered \xNN, defending a tty
 * against control-sequence injection from device-controlled strings. No
 * surrounding quotes; NULL is a no-op. Truncation behavior at the
 * implementation (cli/io.c). */
void mos_cli_safe_ascii(FILE *f, const char *s);

/* Outcome of finalizing stdout after a command has written its output:
 *   MOS_CLI_STDOUT_OK           write succeeded
 *   MOS_CLI_STDOUT_PIPE_CLOSED  EPIPE — downstream closed (`… | head`);
 *                               producer-clean, callers map to EX_OK
 *   MOS_CLI_STDOUT_WRITE_ERROR  any other failure; callers map to EX_IOERR */
typedef enum {
    MOS_CLI_STDOUT_OK = 0,
    MOS_CLI_STDOUT_PIPE_CLOSED,
    MOS_CLI_STDOUT_WRITE_ERROR
} mos_cli_stdout_status;

/* fflush(stdout) + sticky-ferror check, classified per the enum above.
 * Call once after a command has written all its output. */
mos_cli_stdout_status mos_cli_stdout_finalize(void);

#endif /* MOS_CLI_IO_H */
