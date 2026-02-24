/**
 * @file test_suite.c
 * @brief Test registration and execution engine.
 */
#include "test_suite.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

void test_suite_add(test_suite_t *suite, const char *name,
                    const char *desc, const char *spec, test_fn_t fn) {
    if (suite->count >= TEST_MAX_TESTS) return;
    test_entry_t *e = &suite->tests[suite->count++];
    e->name         = name;
    e->description  = desc;
    e->spec_section = spec;
    e->fn           = fn;
}

/** Case-insensitive substring search. */
static bool filter_match(const char *haystack, const char *needle) {
    if (!needle || !needle[0]) return true;
    if (!haystack) return false;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

void test_suite_run(test_suite_t *suite, timing_ctx_t *ctx, char addr,
                    test_result_t *results, size_t *result_count,
                    const char *filter) {
    *result_count = 0;

    /* Count matching tests for progress display */
    size_t total = 0;
    if (filter) {
        for (size_t i = 0; i < suite->count; i++) {
            if (filter_match(suite->tests[i].name, filter) ||
                filter_match(suite->tests[i].description, filter))
                total++;
        }
    } else {
        total = suite->count;
    }

    size_t run_idx = 0;

    for (size_t i = 0; i < suite->count; i++) {
        test_entry_t *e = &suite->tests[i];

        /* Filter: skip non-matching tests */
        if (filter && !filter_match(e->name, filter) &&
                      !filter_match(e->description, filter))
            continue;

        run_idx++;
        printf("  [%2zu/%2zu] %-40s ", run_idx, total, e->description);
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
