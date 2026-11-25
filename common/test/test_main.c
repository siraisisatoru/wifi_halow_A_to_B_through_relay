/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test_harness.h"

unsigned test_checks;
unsigned test_failures;

static uint32_t rand_state = 1;

void test_srand(uint32_t seed)
{
    rand_state = seed != 0 ? seed : 1;
}

uint32_t test_rand(void)
{
    /* xorshift32 */
    rand_state ^= rand_state << 13;
    rand_state ^= rand_state >> 17;
    rand_state ^= rand_state << 5;
    return rand_state;
}

int main(int argc, char **argv)
{
    void (*registrars[])(const struct test_case **, size_t *) = {
        test_linkproto_register,
        test_linkstats_register,
        test_msgstream_register,
    };
    const char *filter = argc > 1 ? argv[1] : NULL;
    unsigned run = 0;
    unsigned failed_tests = 0;

    for (size_t r = 0; r < sizeof(registrars) / sizeof(registrars[0]); r++)
    {
        const struct test_case *cases = NULL;
        size_t n = 0;
        registrars[r](&cases, &n);
        for (size_t i = 0; i < n; i++)
        {
            if (filter != NULL && strstr(cases[i].name, filter) == NULL)
            {
                continue;
            }
            unsigned before = test_failures;
            test_srand(0xC0FFEE + (uint32_t)i);
            cases[i].fn();
            run++;
            if (test_failures != before)
            {
                failed_tests++;
                printf("[FAIL] %s\n", cases[i].name);
            }
            else
            {
                printf("[ OK ] %s\n", cases[i].name);
            }
        }
    }

    printf("%u tests, %u checks, %u failed checks, %u failed tests\n",
           run,
           test_checks,
           test_failures,
           failed_tests);
    return (failed_tests == 0 && run > 0) ? 0 : 1;
}
