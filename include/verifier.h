/**
 * @file verifier.h
 * @brief Top-level verifier context and lifecycle.
 */
#ifndef VERIFIER_H
#define VERIFIER_H

/** Tool version — single source for CLI banner and report output. */
#define VERIFIER_VERSION "0.9.0"

#include "hal.h"
#include "timing.h"
#include "test_suite.h"
#include "reporter.h"

/** Operating mode. */
typedef enum {
    MODE_SENSOR_TEST,       /**< Test a real sensor (verifier = master)      */
    MODE_RECORDER_TEST,     /**< Test a data recorder (verifier = sensor)    */
    MODE_MONITOR,           /**< Passive bus monitor / sniffer               */
    MODE_TRANSPARENT,       /**< Interactive transparent command mode        */
} verifier_mode_t;

/** Complete verifier state. */
typedef struct {
    /* Configuration (set by CLI args) */
    const char       *port;
    char              addr;
    verifier_mode_t   mode;
    report_format_t   format;
    const char       *output_file;
    bool              verbose;
    bool              use_rts;
    bool              use_color;
    bool              hex_monitor;   /**< Show raw hex bytes in monitor mode */
    bool              self_test;    /**< Loopback self-test (no hardware)   */
    const char       *test_filter;  /**< Run only tests matching this name (NULL = all) */

    /* Runtime */
    hal_t            *hal;
    timing_ctx_t      timing;
    report_t          report;
} verifier_ctx_t;

/* ── Lifecycle ────────────────────────────────────────────────────── */

int  verifier_init(verifier_ctx_t *ctx);
int  verifier_run(verifier_ctx_t *ctx);
void verifier_cleanup(verifier_ctx_t *ctx);

/* ── Mode-specific runners ────────────────────────────────────────── */

int verifier_run_sensor_tests(verifier_ctx_t *ctx);
int verifier_run_recorder_tests(verifier_ctx_t *ctx);
int verifier_run_monitor(verifier_ctx_t *ctx);
int verifier_run_transparent(verifier_ctx_t *ctx);

/* ── Test suite registration (called by mode runners) ─────────────── */

void sensor_tests_register(test_suite_t *suite);
void recorder_tests_register(test_suite_t *suite);
void selftest_fault_tests_register(test_suite_t *suite); /* --self-test only */

/* Shared response validators (defined in selftest_faults.c, used by the
 * live compliance battery as well): pd.d value grammar per §4.4.8
 * Table 11, and whole-<values>-field validation of a raw aDn! response. */
bool sdi12v_valid_pdd(const char *v, size_t len, char *why, size_t why_sz);
int  sdi12v_validate_values(const char *resp, size_t len,
                            char *why, size_t why_sz,
                            char *bad, size_t bad_sz);

#endif /* VERIFIER_H */
