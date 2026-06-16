/*
 * test_harness.h — minimal test runner. No framework, no deps, one file.
 * See any test_*.c file for usage.
 */

#ifndef MOS_TEST_HARNESS_H
#define MOS_TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Counters defined once in test_main.c. Not `static`: a per-TU copy
   would silently bury failures. */
extern int mos_tests_run;
extern int mos_tests_failed;

#define TEST(name) static int name(void)

#define RUN(name) do {                                               \
    mos_tests_run++;                                                 \
    printf("  %-48s ", #name);                                       \
    fflush(stdout);                                                  \
    int _r = name();                                                 \
    if (_r == 0) {                                                   \
        printf("ok\n");                                              \
    } else {                                                         \
        printf("FAIL\n");                                            \
        mos_tests_failed++;                                          \
    }                                                                \
} while (0)

#define EXPECT(cond) do {                                            \
    if (!(cond)) {                                                   \
        fprintf(stderr, "\n    EXPECT failed at %s:%d: %s\n",        \
                __FILE__, __LINE__, #cond);                          \
        return 1;                                                    \
    }                                                                \
} while (0)

#define EXPECT_EQ(a, b) do {                                         \
    long long _aa = (long long)(a);                                  \
    long long _bb = (long long)(b);                                  \
    if (_aa != _bb) {                                                \
        fprintf(stderr,                                              \
            "\n    EXPECT_EQ failed at %s:%d:\n"                     \
            "      expected: %lld (%s)\n"                            \
            "      actual  : %lld (%s)\n",                           \
            __FILE__, __LINE__, _bb, #b, _aa, #a);                   \
        return 1;                                                    \
    }                                                                \
} while (0)

#define EXPECT_STREQ(a, b) do {                                      \
    const char *_aa = (a);                                           \
    const char *_bb = (b);                                           \
    if (!_aa || !_bb || strcmp(_aa, _bb) != 0) {                     \
        fprintf(stderr,                                              \
            "\n    EXPECT_STREQ failed at %s:%d:\n"                  \
            "      expected: \"%s\"\n"                               \
            "      actual  : \"%s\"\n",                              \
            __FILE__, __LINE__, _bb ? _bb : "(null)",                \
                                 _aa ? _aa : "(null)");              \
        return 1;                                                    \
    }                                                                \
} while (0)

int test_summary(void);

#endif /* MOS_TEST_HARNESS_H */
