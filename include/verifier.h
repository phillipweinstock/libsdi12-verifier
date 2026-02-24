/**
 * @file verifier.h
 * @brief Top-level verifier context and lifecycle.
 */
#ifndef VERIFIER_H
#define VERIFIER_H

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

#endif /* VERIFIER_H */
