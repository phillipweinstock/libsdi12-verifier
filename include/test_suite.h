/**
 * @file test_suite.h
 * @brief Test registration, execution, and result reporting.
 */
#ifndef TEST_SUITE_H
#define TEST_SUITE_H

#include "timing.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define TEST_DETAIL_MAX  512
#define TEST_MAX_TESTS   64

/** Test outcome. */
typedef enum {
    TEST_PASS,
    TEST_FAIL,
    TEST_SKIP,
    TEST_ERROR
} test_status_t;

/** Result of a single test. */
typedef struct {
    const char    *name;
    const char    *key;           /**< Machine-readable test ID (e.g. "meas_m") */
    const char    *spec_section;
    test_status_t  status;
    char           detail[TEST_DETAIL_MAX];

    /* Timing measurements (0 = not a timing test) */
    uint64_t       measured_us;
    uint64_t       spec_limit_us;
} test_result_t;

/** Test function signature: receives timing context + target address. */
typedef test_result_t (*test_fn_t)(timing_ctx_t *ctx, char addr);

/** Registered test entry. */
typedef struct {
    const char  *name;
    const char  *description;
    const char  *spec_section;
    test_fn_t    fn;
} test_entry_t;

/** A named collection of tests. */
typedef struct {
    const char    *name;
    test_entry_t   tests[TEST_MAX_TESTS];
    size_t         count;
} test_suite_t;

/* ── API ──────────────────────────────────────────────────────────── */

/** Register a test in a suite. */
void test_suite_add(test_suite_t *suite, const char *name,
                    const char *desc, const char *spec, test_fn_t fn);

/**
 * Run all tests in a suite, storing results.
 * @param filter  If non-NULL, only run tests whose name contains this
 *                substring (case-insensitive match).
 */
void test_suite_run(test_suite_t *suite, timing_ctx_t *ctx, char addr,
                    test_result_t *results, size_t *result_count,
                    const char *filter);

/* ── Helper macros for writing test functions ─────────────────────── */

#define TEST_RESULT(n, s) \
    (test_result_t){ .name = (n), .key = NULL, .spec_section = (s), \
                     .status = TEST_PASS, .detail = {0}, \
                     .measured_us = 0, .spec_limit_us = 0 }

#define TEST_PASS_MSG(r, fmt, ...) do { \
    (r).status = TEST_PASS; \
    snprintf((r).detail, TEST_DETAIL_MAX, fmt, ##__VA_ARGS__); \
} while(0)

#define TEST_FAIL_MSG(r, fmt, ...) do { \
    (r).status = TEST_FAIL; \
    snprintf((r).detail, TEST_DETAIL_MAX, fmt, ##__VA_ARGS__); \
} while(0)

#define TEST_SKIP_MSG(r, fmt, ...) do { \
    (r).status = TEST_SKIP; \
    snprintf((r).detail, TEST_DETAIL_MAX, fmt, ##__VA_ARGS__); \
} while(0)

#define TEST_ERROR_MSG(r, fmt, ...) do { \
    (r).status = TEST_ERROR; \
    snprintf((r).detail, TEST_DETAIL_MAX, fmt, ##__VA_ARGS__); \
} while(0)

#endif /* TEST_SUITE_H */
