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

/* ---- mos_cli_json_str: the two-pass malloc sizing path ------------------
 * The plain escaper behaviour (NULL→null, the quote pair, \u00XX / \xNN
 * forms, the dev-node rendering) is exercised end-to-end by the macOS
 * emit-validate harness (tests/emit), which drives every CLI emitter
 * through these writers — so re-asserting it here would be redundant on the
 * authoritative combined number. The ONE branch no emit fixture reaches is
 * the heap path: cli/io.c sizes with mos_json_escape, then escapes into a
 * 256-byte stack buffer or malloc beyond it, and every real identity/volume
 * string is far shorter than that. This pins the malloc branch (and that it
 * produces a complete, untruncated result) on the platform-independent
 * suite. open_memstream (POSIX.1-2008) captures the FILE* output. */
TEST(cli_json_str_uses_malloc_path_beyond_stack)
{
    char in[201];
    memset(in, '\\', 200);              /* 200 backslashes → 400 escaped bytes */
    in[200] = '\0';

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) { fprintf(stderr, "\n    open_memstream failed\n"); return 1; }
    mos_cli_json_str(f, in);
    fclose(f);                          /* flushes; NUL-terminates buf */

    char expect[1 + 400 + 1 + 1];       /* quote + 400 + quote + NUL */
    expect[0] = '"';
    memset(expect + 1, '\\', 400);
    expect[401] = '"';
    expect[402] = '\0';

    int bad = (buf == NULL) || strcmp(buf, expect) != 0;
    if (bad)
        fprintf(stderr, "\n    malloc-path mismatch: got \"%s\"\n",
                buf ? buf : "(null)");
    free(buf);
    return bad;
}

TEST(cli_safe_ascii_renders_human_escapes)
{
    /* mos_cli_safe_ascii is the HUMAN-output escaper (unquoted \xNN form);
       the macOS emit-validate harness drives only the --json path, so it
       never reaches this writer — the combined coverage shows its body red.
       Pin it on the platform-independent suite: NULL writes nothing, plain
       passes through, control bytes render as \xNN. */
    static const struct { const char *in, *expect; } cases[] = {
        { NULL,           ""             },   /* NULL → nothing written */
        { "plain",        "plain"        },
        { "a\x01\x1f" "b", "a\\x01\\x1fb" },  /* control bytes → \xNN */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char *buf = NULL;
        size_t len = 0;
        FILE *f = open_memstream(&buf, &len);
        if (!f) { fprintf(stderr, "\n    open_memstream failed\n"); return 1; }
        mos_cli_safe_ascii(f, cases[i].in);
        fclose(f);
        int bad = (buf == NULL) || strcmp(buf, cases[i].expect) != 0;
        if (bad)
            fprintf(stderr, "\n    safe_ascii[%zu]: expected \"%s\", got \"%s\"\n",
                    i, cases[i].expect, buf ? buf : "(null)");
        free(buf);
        if (bad) return 1;
    }
    return 0;
}

void register_io_tests(void)
{
    RUN(stdout_finalize_epipe_classifies_pipe_closed);
    RUN(stdout_finalize_other_error_classifies_write_error);
    RUN(cli_json_str_uses_malloc_path_beyond_stack);
    RUN(cli_safe_ascii_renders_human_escapes);
}
