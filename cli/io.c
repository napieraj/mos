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
    /* Size first, then escape into an exactly-sized buffer: a fixed cap
       could truncate mid-escape (after a backslash, inside a \uXXXX) and
       emit invalid JSON. A stack buffer covers the common short fields;
       longer strings fall back to malloc. */
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
    /* Fixed 4 KiB buffer; input beyond it is silently dropped. Every call
       site (argv, env values, SPC identity strings) is far below that, and
       \xNN truncation cannot split into something dangerous the way JSON
       mid-escape truncation can — so no two-pass sizing here. */
    char buf[4096];
    mos_safe_ascii(s, buf, sizeof buf);
    fputs(buf, f);
}

void mos_cli_bsd_dev_node(FILE *f, int64_t unit)
{
    char path[24];     /* fits "/dev/disk4294967295" */
    /* mos_bsd_dev_node refuses unit < 0 (no media) and units above
       UINT32_MAX, so the result is `null` or a valid pasteable node,
       never a corrupted one. */
    if (!mos_bsd_dev_node(unit, path, sizeof path)) {
        fputs("null", f);
        return;
    }
    mos_cli_json_str(f, path);          /* emits "/dev/diskN" */
}

mos_cli_stdout_status mos_cli_stdout_finalize(void)
{
    /* ferror is sticky, so a failure that latched in an earlier buffered
       write is still visible here, not just one in this final flush. errno
       is read without clearing: whenever ferror is set or fflush returns
       EOF, a write/flush syscall failed during this emission and set errno,
       and the no-op writes after a latch don't touch it — so errno is fresh
       at the classification. Clearing it ourselves would make correctness
       depend on fflush re-setting errno (glibc-true, not C/POSIX-guaranteed);
       clearerr() would drop the sticky signal and risk a false OK. */
    if (fflush(stdout) == 0 && !ferror(stdout)) return MOS_CLI_STDOUT_OK;
    return (errno == EPIPE) ? MOS_CLI_STDOUT_PIPE_CLOSED
                            : MOS_CLI_STDOUT_WRITE_ERROR;
}
