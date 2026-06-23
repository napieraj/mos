/*
 * cli/io.c — shared CLI string writers; contracts in io.h.
 */
#include "io.h"

#include "mos.h"   /* mos_json_escape, mos_safe_ascii */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

/* ---- JSON -> dotted key=value flattening (`--pairs`) ----------------- *
 * A tolerant recursive-descent walk over mos's own well-formed JSON. It
 * never DECODES a scalar — it emits the raw JSON span (strings keep quotes
 * + escapes, so each value stays single-line and shell-parseable). Bounded
 * by the NUL terminator; malformed input stops the walk (returns NULL up
 * the stack) so the caller can fall back. */

static const char *jp_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* p at the opening quote; return just past the closing quote, or NULL. */
static const char *jp_string_end(const char *p)
{
    p++;                                   /* skip opening quote */
    while (*p) {
        if (*p == '\\') { if (!p[1]) return NULL; p += 2; continue; }
        if (*p == '"')  return p + 1;
        p++;
    }
    return NULL;
}

static const char *jp_value(const char *p, const char *path, FILE *out)
{
    p = jp_skip_ws(p);

    if (*p == '{') {
        p = jp_skip_ws(p + 1);
        if (*p == '}') {                   /* empty object — keep the key */
            if (*path) fprintf(out, "%s={}\n", path);
            return p + 1;
        }
        for (;;) {
            p = jp_skip_ws(p);
            if (*p != '"') return NULL;
            const char *kend = jp_string_end(p);
            if (!kend) return NULL;
            char key[256];
            size_t klen = (size_t)(kend - p) - 2;     /* strip the quotes */
            if (klen >= sizeof key) klen = sizeof key - 1;
            memcpy(key, p + 1, klen);
            key[klen] = '\0';
            char child[512];
            if (*path) snprintf(child, sizeof child, "%s.%s", path, key);
            else       snprintf(child, sizeof child, "%s", key);
            p = jp_skip_ws(kend);
            if (*p != ':') return NULL;
            p = jp_value(p + 1, child, out);
            if (!p) return NULL;
            p = jp_skip_ws(p);
            if (*p == ',') { p++; continue; }
            if (*p == '}') return p + 1;
            return NULL;
        }
    }

    if (*p == '[') {
        p = jp_skip_ws(p + 1);
        if (*p == ']') {                   /* empty array — keep the key */
            if (*path) fprintf(out, "%s=[]\n", path);
            return p + 1;
        }
        for (int i = 0; ; i++) {
            char child[512];
            snprintf(child, sizeof child, "%s[%d]", path, i);
            p = jp_value(p, child, out);
            if (!p) return NULL;
            p = jp_skip_ws(p);
            if (*p == ',') { p++; continue; }
            if (*p == ']') return p + 1;
            return NULL;
        }
    }

    if (*p == '"') {                       /* string scalar — raw, with quotes */
        const char *end = jp_string_end(p);
        if (!end) return NULL;
        fprintf(out, "%s=", path);
        fwrite(p, 1, (size_t)(end - p), out);
        fputc('\n', out);
        return end;
    }

    /* number / true / false / null — raw span to the next delimiter. */
    const char *start = p;
    while (*p && *p != ',' && *p != ']' && *p != '}' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    if (p == start) return NULL;
    fprintf(out, "%s=", path);
    fwrite(start, 1, (size_t)(p - start), out);
    fputc('\n', out);
    return p;
}

bool mos_cli_json_to_pairs(const char *json, FILE *out)
{
    if (!json || !out) return false;
    const char *p = jp_value(json, "", out);
    if (!p) return false;
    p = jp_skip_ws(p);
    return *p == '\0';                      /* trailing garbage => malformed */
}

void mos_cli_json_str(FILE *f, const char *s)
{
    if (!s) { fputs("null", f); return; }
    /* Size first, then escape into an exactly-sized buffer: a fixed cap
       could truncate mid-escape (after a backslash, inside a \uXXXX) and
       emit invalid JSON. Stack buffer for short fields, malloc beyond. */
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
    /* Fixed 4 KiB buffer; input beyond it is dropped. Every call site
       (argv, env, SPC identity strings) is far below that, and \xNN
       truncation can't split into something dangerous the way JSON mid-
       escape truncation can — so no two-pass sizing. */
    char buf[4096];
    mos_safe_ascii(s, buf, sizeof buf);
    fputs(buf, f);
}

void mos_cli_bsd_dev_node(FILE *f, int64_t unit)
{
    char path[24];     /* fits "/dev/disk4294967295" */
    /* mos_bsd_dev_node refuses unit < 0 (no media) and units above
       UINT32_MAX, so the result is `null` or a valid node, never a
       corrupted one. */
    if (!mos_bsd_dev_node(unit, path, sizeof path)) {
        fputs("null", f);
        return;
    }
    mos_cli_json_str(f, path);          /* emits "/dev/diskN" */
}

mos_cli_stdout_status mos_cli_stdout_finalize(void)
{
    /* ferror is sticky, so a failure latched in an earlier buffered write
       still shows here, not just one from this final flush. errno is read
       without clearing: when ferror is set or fflush returns EOF, a syscall
       failed during this emission and set it, and the no-op writes after a
       latch leave it untouched — so it's fresh at classification. Clearing
       it would make us depend on fflush re-setting errno (glibc-true, not
       C/POSIX-guaranteed); clearerr() would drop the sticky signal and risk
       a false OK. */
    if (fflush(stdout) == 0 && !ferror(stdout)) return MOS_CLI_STDOUT_OK;
    return (errno == EPIPE) ? MOS_CLI_STDOUT_PIPE_CLOSED
                            : MOS_CLI_STDOUT_WRITE_ERROR;
}
