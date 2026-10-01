#ifndef TSIM_TESTS_CHECK_H
#define TSIM_TESTS_CHECK_H

/* The smallest test harness that does the job: a failed check prints where and why, counts, and
 * carries on, and main() returns non-zero if anything failed. Each tests/<name>.c is one
 * executable and one CTest entry. */

#include <inttypes.h>
#include <stdio.h>

static int check_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);               \
            check_failures++;                                                                      \
        }                                                                                          \
    } while (0)

#define CHECK_EQ_I64(actual, expected)                                                             \
    do {                                                                                           \
        int64_t a_ = (int64_t)(actual);                                                            \
        int64_t e_ = (int64_t)(expected);                                                          \
        if (a_ != e_) {                                                                            \
            fprintf(stderr, "%s:%d: %s == %" PRId64 ", expected %" PRId64 "\n", __FILE__,          \
                    __LINE__, #actual, a_, e_);                                                    \
            check_failures++;                                                                      \
        }                                                                                          \
    } while (0)

#define CHECK_EQ_U64(actual, expected)                                                             \
    do {                                                                                           \
        uint64_t a_ = (uint64_t)(actual);                                                          \
        uint64_t e_ = (uint64_t)(expected);                                                        \
        if (a_ != e_) {                                                                            \
            fprintf(stderr, "%s:%d: %s == 0x%016" PRIx64 ", expected 0x%016" PRIx64 "\n",          \
                    __FILE__, __LINE__, #actual, a_, e_);                                          \
            check_failures++;                                                                      \
        }                                                                                          \
    } while (0)

#define RUN(test)                                                                                  \
    do {                                                                                           \
        int before_ = check_failures;                                                              \
        test();                                                                                    \
        printf("%s %s\n", check_failures == before_ ? "ok  " : "FAIL", #test);                     \
    } while (0)

#define CHECK_DONE() (check_failures ? 1 : 0)

#endif
