/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
#ifndef RFM_TEST_CHECK_H
#define RFM_TEST_CHECK_H
#include <stdio.h>

static int g_checks, g_fails;
#define CHECK(c)                                                                                                    \
    do {                                                                                                            \
        g_checks++;                                                                                                 \
        if (!(c)) {                                                                                                 \
            g_fails++;                                                                                              \
            if (g_fails < 30)                                                                                       \
                fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                              \
        }                                                                                                           \
    } while (0)
#define CHECKF(c, ...)                                                                                              \
    do {                                                                                                            \
        g_checks++;                                                                                                 \
        if (!(c)) {                                                                                                 \
            g_fails++;                                                                                              \
            if (g_fails < 30) {                                                                                     \
                fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #c);                               \
                fprintf(stderr, __VA_ARGS__);                                                                       \
                fputc('\n', stderr);                                                                                \
            }                                                                                                       \
        }                                                                                                           \
    } while (0)
static int check_done(const char *name)
{
    printf("%s: %d checks, %d failed\n", name, g_checks, g_fails);
    return g_fails ? 1 : 0;
}
#endif
