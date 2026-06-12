/*
 * cli/io.c — shared CLI string writers; contracts in io.h.
 */
#include "io.h"

#include "mos.h"   /* mos_json_escape, mos_safe_ascii */

#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

void mos_cli_json_str(FILE *f, const char *s)
{
    if (!s) { fputs("null", f); return; }
    /* Size first (out=NULL,cap=0 returns the escaped length excluding the
       NUL), then escape into an exactly-sized buffer — a fixed-cap buffer
       would truncate, and truncation can land mid-escape (after a
       backslash, or inside a \uXXXX), emitting invalid JSON. A small stack
       buffer covers the common short fields; longer strings (e.g. CF
       properties via the notification probe) fall back to malloc. */
    char stack[256];
    size_t need = mos_json_escape(s, NULL, 0);
    char *buf = stack;
    if (need > SIZE_MAX - 1) { fputs("\"<oom>\"", f); return; }
    if (need + 1 > sizeof stack) {
        buf = malloc(need + 1);
        if (!buf) { fputs("\"<oom>\"", f); return; }
    }
    mos_json_escape(s, buf, need + 1);
    fputc('"', f);
    fputs(buf, f);
    fputc('"', f);
    if (buf != stack) free(buf);
}

void mos_cli_safe_ascii(FILE *f, const char *s)
{
    if (!s) return;
    char buf[4096];
    mos_safe_ascii(s, buf, sizeof buf);
    fputs(buf, f);
}

void mos_cli_bsd_dev_node(FILE *f, int64_t unit)
{
    char path[24];
    /* The JSON `bsd` field carries the full canonical device node
       ("/dev/diskN") — pasteable, pipeable, always a valid `--bsd`
       argument when non-null (doc/research/2026-06-10-cli-design.md).
       mos_bsd_dev_node refuses unit < 0 (no media) and units above
       UINT32_MAX, so the value is null or a valid node — never a
       corrupted one. (24 bytes always holds an in-domain unit:
       "/dev/disk4294967295".) */
    if (!mos_bsd_dev_node(unit, path, sizeof path)) {
        fputs("null", f);
        return;
    }
    mos_cli_json_str(f, path);          /* emits "/dev/diskN" */
}

mos_cli_stdout_status mos_cli_stdout_finalize(void)
{
    /* ferror is the authoritative "did a write fail" signal: it is sticky,
       so a failure that latched in an earlier buffered fputs/fputc/fprintf
       (or a buffer-full auto-flush) is still visible here, not only one in
       this final flush.

       errno only distinguishes EPIPE from other failures, and is read
       WITHOUT being cleared: whenever ferror is set OR fflush returns EOF,
       some write/flush syscall failed during this emission and set errno —
       and the no-op writes that follow a latch (stdio short-circuits once
       the error flag is set) do not touch it. So errno is fresh whenever
       we reach the classification line. We do not clear errno ourselves:
       clearing it immediately before fflush would make correct
       classification depend on fflush re-setting errno after a latch that
       already emptied the buffer — true on glibc, but not guaranteed by
       C/POSIX, and a needless dependency. (We also never clearerr(): that
       would drop the sticky failure signal and could report a false OK.) */
    if (fflush(stdout) == 0 && !ferror(stdout)) return MOS_CLI_STDOUT_OK;
    return (errno == EPIPE) ? MOS_CLI_STDOUT_PIPE_CLOSED
                            : MOS_CLI_STDOUT_WRITE_ERROR;
}
