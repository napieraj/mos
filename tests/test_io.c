/*
 * test_io.c — mos_cli_stdout_finalize classification (cli/io.c).
 *
 * The finalize routine's errno-freshness argument is reasoned in a long
 * comment but was otherwise untested: after a write/flush latches
 * ferror, the no-op writes that follow MUST NOT disturb errno, so the
 * EPIPE-vs-other classification reached afterwards is still reading the
 * errno the failing syscall set. This pins both branches against a
 * genuinely broken stdout.
 *
 * Each case runs in a FORKED CHILD whose stdout fd is redirected to a
 * broken target, so the parent's stdout (the harness's own ok/FAIL
 * output) is never disturbed. The child overflows any stdio buffer
 * (forcing a failed auto-flush plus the trailing no-op writes the
 * argument is about), calls mos_cli_stdout_finalize, and _exit()s with
 * the classification as its status for the parent to read back. POSIX
 * only, so it runs on Linux CI as well as macOS.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

#include "test_harness.h"
#include "../cli/io.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

typedef enum { BREAK_EPIPE, BREAK_BADFD } break_mode;

/* Run mos_cli_stdout_finalize against a deliberately broken stdout in a
   child process. Returns the child's classification (the enum value) as
   its exit status, or a negative sentinel on a harness-side failure
   (-1 = child died by signal, e.g. SIGPIPE not ignored). */
static int finalize_with_broken_stdout(break_mode mode)
{
    fflush(stdout);                  /* don't let the child inherit buffered
                                        parent (harness) output */
    pid_t pid = fork();
    if (pid < 0) return -2;
    if (pid == 0) {
        /* Mirror the real CLI: SIGPIPE is ignored process-wide at main()
           entry (cli/main.c), so a write to a closed pipe yields EPIPE in
           errno rather than killing the process. Without this the EPIPE
           branch would be unreachable — the test would catch that as a
           signal death (-1). */
        signal(SIGPIPE, SIG_IGN);

        int target;
        if (mode == BREAK_EPIPE) {
            int p[2];
            if (pipe(p) != 0) _exit(120);
            close(p[0]);             /* reader gone → writes get EPIPE */
            target = p[1];
        } else {
            target = open("/dev/null", O_RDONLY);   /* writes get EBADF */
            if (target < 0) _exit(121);
        }
        if (dup2(target, STDOUT_FILENO) < 0) _exit(122);

        /* Far past any stdio buffer: guarantees a failed auto-flush that
           latches ferror + errno, then the no-op writes the
           errno-freshness argument is about, before finalize's own
           fflush. */
        for (int i = 0; i < 100000; i++) putc('x', stdout);

        mos_cli_stdout_status st = mos_cli_stdout_finalize();
        _exit((int)st);             /* _exit: no stdio flush, no atexit */
    }

    int ws = 0;
    if (waitpid(pid, &ws, 0) < 0) return -3;
    if (WIFSIGNALED(ws)) return -1;
    return WEXITSTATUS(ws);
}

TEST(stdout_finalize_epipe_classifies_pipe_closed)
{
    int st = finalize_with_broken_stdout(BREAK_EPIPE);
    EXPECT(st >= 0);                 /* not a signal death / fork failure */
    EXPECT_EQ(st, MOS_CLI_STDOUT_PIPE_CLOSED);
    return 0;
}

TEST(stdout_finalize_other_error_classifies_write_error)
{
    int st = finalize_with_broken_stdout(BREAK_BADFD);
    EXPECT(st >= 0);
    EXPECT_EQ(st, MOS_CLI_STDOUT_WRITE_ERROR);
    return 0;
}

/* ---- CLI string writers (mos_cli_json_str / _safe_ascii / _bsd_dev_node) --
 * These wrap the pure mos_json_escape / mos_safe_ascii (covered in
 * test_render.c) with the CLI's FILE* framing: NULL handling, the quote
 * pair, the malloc-beyond-stack sizing path, and the dev-node rendering.
 * open_memstream (POSIX.1-2008) captures the writer's output for exact
 * comparison; runs on Linux CI and macOS alike. */

/* Run `writer` into an open_memstream and compare the captured bytes to
   `expect`. Returns 0 on match, 1 otherwise (with a harness diagnostic). */
typedef void (*str_writer)(FILE *, const char *);
static int expect_str_capture(str_writer writer, const char *in,
                              const char *expect, int line)
{
    char  *buf = NULL;
    size_t len = 0;
    FILE  *f   = open_memstream(&buf, &len);
    if (!f) { fprintf(stderr, "\n    open_memstream failed\n"); return 1; }
    writer(f, in);
    fclose(f);                          /* flushes; NUL-terminates buf */
    int bad = (buf == NULL) || strcmp(buf, expect) != 0;
    if (bad)
        fprintf(stderr,
            "\n    capture mismatch at %s:%d:\n"
            "      expected: \"%s\"\n      actual  : \"%s\"\n",
            __FILE__, line, expect, buf ? buf : "(null)");
    free(buf);
    return bad;
}
#define EXPECT_CAPTURE(writer, in, expect) do {                       \
    if (expect_str_capture((writer), (in), (expect), __LINE__))      \
        return 1;                                                     \
} while (0)

TEST(cli_json_str_null_plain_and_escapes)
{
    EXPECT_CAPTURE(mos_cli_json_str, NULL, "null");      /* NULL → bare null */
    EXPECT_CAPTURE(mos_cli_json_str, "", "\"\"");        /* empty → "" */
    EXPECT_CAPTURE(mos_cli_json_str, "plain", "\"plain\"");
    /* quote + backslash named-escaped, the whole wrapped in quotes */
    EXPECT_CAPTURE(mos_cli_json_str, "a\"b\\c", "\"a\\\"b\\\\c\"");
    /* unnamed control byte → \u00XX (lowercase), matching mos_json_escape */
    EXPECT_CAPTURE(mos_cli_json_str, "x\x01y", "\"x\\u0001y\"");
    return 0;
}

TEST(cli_json_str_uses_malloc_path_beyond_stack)
{
    /* cli/io.c sizes with mos_json_escape then escapes into an exactly-sized
       buffer, switching from a 256-byte stack buffer to malloc past it. Drive
       a string whose escaped form exceeds 256 so the malloc branch runs and
       still produces a complete, correctly quoted result (no mid-escape
       truncation). 200 backslashes escape to 400 bytes. */
    char in[201];
    memset(in, '\\', 200);
    in[200] = '\0';
    char expect[1 + 400 + 1 + 1];       /* quote + 400 + quote + NUL */
    expect[0] = '"';
    memset(expect + 1, '\\', 400);
    expect[401] = '"';
    expect[402] = '\0';
    EXPECT_CAPTURE(mos_cli_json_str, in, expect);
    return 0;
}

TEST(cli_safe_ascii_null_plain_and_escapes)
{
    EXPECT_CAPTURE(mos_cli_safe_ascii, NULL, "");        /* NULL → nothing */
    EXPECT_CAPTURE(mos_cli_safe_ascii, "plain", "plain");
    /* control bytes → \xNN (no surrounding quotes — unquoted human form) */
    EXPECT_CAPTURE(mos_cli_safe_ascii, "a\x01\x1f" "b", "a\\x01\\x1fb");
    return 0;
}

/* mos_cli_bsd_dev_node takes an int64_t unit, not a string, so it needs its
   own capture (it renders "/dev/diskN" as a JSON string, or bare null). */
static int expect_node_capture(int64_t unit, const char *expect, int line)
{
    char  *buf = NULL;
    size_t len = 0;
    FILE  *f   = open_memstream(&buf, &len);
    if (!f) { fprintf(stderr, "\n    open_memstream failed\n"); return 1; }
    mos_cli_bsd_dev_node(f, unit);
    fclose(f);
    int bad = (buf == NULL) || strcmp(buf, expect) != 0;
    if (bad)
        fprintf(stderr,
            "\n    node mismatch at %s:%d: expected \"%s\", got \"%s\"\n",
            __FILE__, line, expect, buf ? buf : "(null)");
    free(buf);
    return bad;
}

TEST(cli_bsd_dev_node_renders_node_or_null)
{
    /* Valid unit → quoted dev node. */
    if (expect_node_capture(4, "\"/dev/disk4\"", __LINE__)) return 1;
    /* unit < 0 is the "no media" sentinel → bare null (not a node). */
    if (expect_node_capture(-1, "null", __LINE__)) return 1;
    return 0;
}

void register_io_tests(void)
{
    RUN(stdout_finalize_epipe_classifies_pipe_closed);
    RUN(stdout_finalize_other_error_classifies_write_error);
    RUN(cli_json_str_null_plain_and_escapes);
    RUN(cli_json_str_uses_malloc_path_beyond_stack);
    RUN(cli_safe_ascii_null_plain_and_escapes);
    RUN(cli_bsd_dev_node_renders_node_or_null);
}
