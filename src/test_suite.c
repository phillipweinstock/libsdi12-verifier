/**
 * @file test_suite.c
 * @brief Test registration and execution engine.
 */
#include "test_suite.h"
#include <stdio.h>
#include <string.h>

void test_suite_add(test_suite_t *suite, const char *name,
                    const char *desc, const char *spec, test_fn_t fn) {
    if (suite->count >= TEST_MAX_TESTS) return;
    test_entry_t *e = &suite->tests[suite->count++];
    e->name         = name;
    e->description  = desc;
    e->spec_section = spec;
    e->fn           = fn;
}

void test_suite_run(test_suite_t *suite, timing_ctx_t *ctx, char addr,
                    test_result_t *results, size_t *result_count) {
    *result_count = 0;

    for (size_t i = 0; i < suite->count; i++) {
        test_entry_t *e = &suite->tests[i];

        printf("  [%2zu/%2zu] %-40s ", i + 1, suite->count, e->description);
        fflush(stdout);

        test_result_t r = e->fn(ctx, addr);

        /* Ensure name and spec are set from registration if not by test */
        if (!r.name)         r.name = e->description;
        if (!r.spec_section) r.spec_section = e->spec_section;

        switch (r.status) {
            case TEST_PASS:  printf("PASS");  break;
            case TEST_FAIL:  printf("FAIL");  break;
            case TEST_SKIP:  printf("SKIP");  break;
            case TEST_ERROR: printf("ERROR"); break;
        }

        if (r.detail[0])
            printf(" -- %s", r.detail);

        printf("\n");

        results[(*result_count)++] = r;
    }

    printf("\n");
}
