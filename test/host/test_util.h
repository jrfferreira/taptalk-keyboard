/* Dependency-free test scaffolding. There is no cmake on the dev machine, so
 * this is a plain Makefile + clang setup rather than CTest. */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run    = 0;
static int tests_failed = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        tests_run++;                                                                     \
        if (!(cond)) {                                                                   \
            tests_failed++;                                                              \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                                \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                               \
    do {                                                                                 \
        tests_run++;                                                                     \
        long long _a = (long long)(a), _b = (long long)(b);                              \
        if (_a != _b) {                                                                  \
            tests_failed++;                                                              \
            fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__,         \
                    __LINE__, #a, #b, _a, _b);                                           \
        }                                                                                \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                               \
    do {                                                                                 \
        tests_run++;                                                                     \
        if (strcmp((a), (b)) != 0) {                                                     \
            tests_failed++;                                                              \
            fprintf(stderr, "  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,      \
                    (a), (b));                                                           \
        }                                                                                \
    } while (0)

#define TEST_MAIN(name, body)                                                            \
    int main(void)                                                                       \
    {                                                                                    \
        body;                                                                            \
        if (tests_failed == 0) {                                                         \
            printf("PASS %-14s %3d checks\n", (name), tests_run);                        \
            return 0;                                                                    \
        }                                                                                \
        printf("FAIL %-14s %d/%d checks failed\n", (name), tests_failed, tests_run);     \
        return 1;                                                                        \
    }
