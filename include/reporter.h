/**
 * @file reporter.h
 * @brief Compliance report generation — text and JSON output.
 */
#ifndef REPORTER_H
#define REPORTER_H

#include "test_suite.h"
#include <stdio.h>

/** Output format. */
typedef enum {
    REPORT_TEXT,
    REPORT_JSON
} report_format_t;

/** Accumulated test results for a complete report. */
typedef struct {
    test_result_t  results[TEST_MAX_TESTS * 4];
    size_t         count;
    const char    *suite_name;
    const char    *device_port;
    char           device_addr;
    const char    *sdi12_version;
} report_t;

void report_init(report_t *rpt, const char *suite, const char *port, char addr);
void report_add_result(report_t *rpt, const test_result_t *result);
void report_add_results(report_t *rpt, const test_result_t *results, size_t count);
void report_print_text(const report_t *rpt, FILE *out);
void report_print_json(const report_t *rpt, FILE *out);
void report_print(const report_t *rpt, report_format_t fmt, FILE *out);
void report_summary(const report_t *rpt, size_t *pass, size_t *fail,
                    size_t *skip, size_t *error);

#endif /* REPORTER_H */
