/*
 * cli/io.h — shared stdout/stderr writers for the mos command-line
 * tools (mos, mos_probe, mos_notification_probe).
 *
 * The JSON / ASCII escaping *rule* lives once in the library
 * (mos_json_escape / mos_safe_ascii, src/mos_strings.c); these are the
 * thin FILE-writing wrappers around it — one quoting JSON-value writer,
 * one ASCII-safe writer — so every tool emits identically.
 */
#ifndef MOS_CLI_IO_H
#define MOS_CLI_IO_H

#include <stdio.h>
#include <stdint.h>

/* Write `s` to `f` as a complete JSON string value: a double-quoted,
 * mos_json_escape'd token, OR the bare literal `null` when s == NULL.
 * The surrounding quotes are part of the output — callers must NOT add
 * their own. This is the single canonical JSON-value emitter for the
 * CLI tools (e.g. a NULL vendor/product from a drive the device
 * directory carries no identity for renders as `null`, satisfying the
 * nullable schema rather than crashing or emitting ""). */
void mos_cli_json_str(FILE *f, const char *s);

/* Write the whole-disk unit to `f` as a complete JSON string value:
 * the full DEVICE NODE `"/dev/diskN"` for a valid unit in
 * [0, UINT32_MAX] (the form all four schemas pin: ^/dev/disk[0-9]+$),
 * or the bare literal `null` for unit < 0 (no media) or a unit outside
 * that domain (refused by mos_bsd_dev_node's domain guard, never
 * rendered as a valid-looking node).
 * Surrounding quotes are part of the output. Keeps the JSON wire contract
 * a string-or-null even though the drive identity is carried as an
 * integer (Commit D). */
void mos_cli_bsd_dev_node(FILE *f, int64_t unit);

/* Write `s` to `f` with every non-printable byte rendered as \xNN via
 * mos_safe_ascii (defends a tty against control-sequence injection from
 * device-controlled strings). No surrounding quotes. NULL is a no-op. */
void mos_cli_safe_ascii(FILE *f, const char *s);

/* Outcome of finalizing stdout after a tool has written its output.
 *
 * The classification rule (fflush + sticky ferror + best-effort errno)
 * lives here, once, because three call sites need it: the one-shot
 * status/list renderers and the watch event loop.
 *
 *   MOS_CLI_STDOUT_OK           write succeeded
 *   MOS_CLI_STDOUT_PIPE_CLOSED  write failed with EPIPE — the downstream
 *                               consumer closed (e.g. `mos --json | head`).
 *                               Producer-clean: callers map this to EX_OK.
 *   MOS_CLI_STDOUT_WRITE_ERROR  write failed for another reason (ENOSPC,
 *                               EIO, ...). Callers map this to EX_IOERR.
 *
 * errno classification is best-effort: ferror is sticky, so a failure
 * latched earlier in a buffered write sequence is still detected here,
 * but errno may have been overwritten between that latch and the final
 * fflush. We therefore trust ferror for "did a write fail" and use errno
 * only to distinguish EPIPE from other failures — never clearing errno
 * ourselves, which could misclassify a real EPIPE as a generic error. */
typedef enum {
    MOS_CLI_STDOUT_OK = 0,
    MOS_CLI_STDOUT_PIPE_CLOSED,
    MOS_CLI_STDOUT_WRITE_ERROR
} mos_cli_stdout_status;

/* fflush(stdout) + sticky ferror(stdout) check, classified per the enum
 * above. Call once after a command has written all its output. */
mos_cli_stdout_status mos_cli_stdout_finalize(void);

#endif /* MOS_CLI_IO_H */
