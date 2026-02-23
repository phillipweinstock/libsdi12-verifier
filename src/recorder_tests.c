/**
 * @file recorder_tests.c
 * @brief SDI-12 data recorder compliance tests (placeholder).
 *
 * These tests use the libsdi12 sensor API to simulate a configurable
 * sensor, then measure the data recorder's command patterns, timing,
 * and protocol compliance.
 *
 * TODO: Full implementation — requires a different architecture where
 * the verifier acts as sensor (slave) rather than master.
 */
#include "verifier.h"

void recorder_tests_register(test_suite_t *suite) {
    (void)suite;

    /*
     * Planned recorder compliance tests:
     *
     * test_suite_add(suite, "rec_break",     "Break Duration >=12ms",       "§4.2.1", ...);
     * test_suite_add(suite, "rec_marking",   "Marking >=8.33ms",            "§4.2.1", ...);
     * test_suite_add(suite, "rec_cmd_fmt",   "Command Format (7E1)",        "§4.3",   ...);
     * test_suite_add(suite, "rec_sequence",  "M->wait->D Sequence",         "§4.4.6", ...);
     * test_suite_add(suite, "rec_retry",     "Retry on Timeout",            "§4.4.6", ...);
     * test_suite_add(suite, "rec_interchar", "Inter-Command Gap",           "§4.2.4", ...);
     * test_suite_add(suite, "rec_address",   "Correct Address in Commands", "§4.3",   ...);
     */
}
