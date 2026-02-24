/**
 * @file sensor_tests.c
 * @brief SDI-12 sensor compliance tests (31 tests).
 *
 * Each test exercises a specific aspect of the SDI-12 v1.4 specification.
 * Tests use the libsdi12 master API through the timing interposition
 * layer to measure both correctness and timing compliance.
 *
 * Spec section references follow the SDI-12 v1.4 (February 2023) document.
 */
#include "verifier.h"
#include "sdi12.h"
#include "sdi12_master.h"
#include <stdio.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────── */

static bool valid_sdi12_addr(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

/* ══════════════════════════════════════════════════════════════════════
 *  1. Acknowledge (a!)
 *  §4.4.2 — Sensor must respond with "a\r\n"
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_acknowledge(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Acknowledge (a!)", "\xc2\xa7" "4.4.2");

    sdi12_master_send_break(timing_master(ctx));

    bool present = false;
    sdi12_err_t err = sdi12_master_acknowledge(timing_master(ctx), addr, &present);

    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "Command failed with error %d", err);
        return r;
    }
    if (!present) {
        TEST_FAIL_MSG(r, "No response from sensor at address '%c'", addr);
        return r;
    }

    r.measured_us   = timing_response_latency_us(ctx);
    r.spec_limit_us = SDI12_RESPONSE_TIMEOUT_MS * 1000;

    if (r.measured_us > r.spec_limit_us) {
        TEST_FAIL_MSG(r, "Response too slow: %.3f ms (max %.3f ms)",
                      r.measured_us / 1000.0, r.spec_limit_us / 1000.0);
        return r;
    }

    TEST_PASS_MSG(r, "Acknowledged in %.3f ms", r.measured_us / 1000.0);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  2. Query Address (?!)
 *  §4.4.1 — Returns "a\r\n" (single sensor on bus)
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_query_address(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Query Address (?!)", "\xc2\xa7" "4.4.1");

    sdi12_master_send_break(timing_master(ctx));

    char queried = 0;
    sdi12_err_t err = sdi12_master_query_address(timing_master(ctx), &queried);

    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "Query failed with error %d", err);
        return r;
    }
    if (!valid_sdi12_addr(queried)) {
        TEST_FAIL_MSG(r, "Invalid address returned: 0x%02X", (unsigned char)queried);
        return r;
    }
    if (queried != addr) {
        TEST_FAIL_MSG(r, "Expected '%c', got '%c'", addr, queried);
        return r;
    }

    r.measured_us   = timing_response_latency_us(ctx);
    r.spec_limit_us = SDI12_RESPONSE_TIMEOUT_MS * 1000;

    TEST_PASS_MSG(r, "Address '%c' confirmed in %.3f ms", queried, r.measured_us / 1000.0);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  3. Identify (aI!)
 *  §4.4.3 — Response: "allccccccccmmmmmmvvvxxxxxxxxx...\r\n"
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_identify(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Identify (aI!)", "\xc2\xa7" "4.4.3");

    sdi12_master_send_break(timing_master(ctx));

    sdi12_ident_t ident;
    sdi12_err_t err = sdi12_master_identify(timing_master(ctx), addr, &ident);

    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "Identify failed with error %d", err);
        return r;
    }

    /* Check vendor is not empty */
    bool vendor_empty = true;
    for (int i = 0; i < SDI12_ID_VENDOR_LEN; i++) {
        if (ident.vendor[i] != '\0' && ident.vendor[i] != ' ') {
            vendor_empty = false;
            break;
        }
    }
    if (vendor_empty) {
        TEST_FAIL_MSG(r, "Vendor field is empty");
        return r;
    }

    /* Check model is not empty */
    bool model_empty = true;
    for (int i = 0; i < SDI12_ID_MODEL_LEN; i++) {
        if (ident.model[i] != '\0' && ident.model[i] != ' ') {
            model_empty = false;
            break;
        }
    }
    if (model_empty) {
        TEST_FAIL_MSG(r, "Model field is empty");
        return r;
    }

    r.measured_us   = timing_response_latency_us(ctx);
    r.spec_limit_us = SDI12_RESPONSE_TIMEOUT_MS * 1000;

    TEST_PASS_MSG(r, "Vendor=%.8s Model=%.6s FW=%.3s Serial=%s",
                  ident.vendor, ident.model, ident.firmware_version, ident.serial);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  4. Wrong Address Silence
 *  §4.3 — Sensor must NOT respond to commands for other addresses
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_wrong_address_silence(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Wrong Address Silence", "\xc2\xa7" "4.3");

    char wrong = (addr == '0') ? '1' : '0';

    sdi12_master_send_break(timing_master(ctx));

    bool present = false;
    sdi12_master_acknowledge(timing_master(ctx), wrong, &present);

    if (present) {
        TEST_FAIL_MSG(r, "Sensor responded to wrong address '%c'", wrong);
        return r;
    }

    TEST_PASS_MSG(r, "Correctly silent for address '%c'", wrong);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  5. Response Timing (<=15ms)
 *  §4.2.3 — Sensor must respond within 15ms of command stop bit
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_response_timing(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Response Timing (<=15ms)", "\xc2\xa7" "4.2.3");

    sdi12_master_send_break(timing_master(ctx));

    bool present = false;
    sdi12_master_acknowledge(timing_master(ctx), addr, &present);

    if (!present) {
        TEST_ERROR_MSG(r, "No response, cannot measure timing");
        return r;
    }

    r.measured_us   = timing_response_latency_us(ctx);
    r.spec_limit_us = SDI12_RESPONSE_TIMEOUT_MS * 1000;

    if (r.measured_us > r.spec_limit_us) {
        TEST_FAIL_MSG(r, "%.3f ms exceeds 15ms limit", r.measured_us / 1000.0);
        return r;
    }

    TEST_PASS_MSG(r, "%.3f ms (limit: %.3f ms)",
                  r.measured_us / 1000.0, r.spec_limit_us / 1000.0);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  6. Inter-Character Gap (<=1.66ms)
 *  §4.2.4 — Max gap between characters within a response
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_interchar_gap(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Inter-Character Gap (<=1.66ms)", "\xc2\xa7" "4.2.4");

    sdi12_master_send_break(timing_master(ctx));

    /* Use identify for a longer multi-character response */
    sdi12_ident_t ident;
    sdi12_err_t err = sdi12_master_identify(timing_master(ctx), addr, &ident);

    if (err != SDI12_OK) {
        TEST_ERROR_MSG(r, "Identify failed, cannot measure interchar gap");
        return r;
    }

    r.measured_us   = timing_max_interchar_gap_us(ctx);
    r.spec_limit_us = SDI12_INTERCHAR_MAX_MS * 1000;

    if (r.measured_us > r.spec_limit_us) {
        TEST_FAIL_MSG(r, "Max gap %.3f ms exceeds %.3f ms limit",
                      r.measured_us / 1000.0, r.spec_limit_us / 1000.0);
        return r;
    }

    TEST_PASS_MSG(r, "Max gap: %.3f ms (limit: %.3f ms)",
                  r.measured_us / 1000.0, r.spec_limit_us / 1000.0);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  7. Standard Measurement (aM!)
 *  §4.4.6 — Response: "atttn\r\n" (ttt=0-999, n=0-9)
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_measurement_m(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Standard Measurement (aM!)", "\xc2\xa7" "4.4.6");

    sdi12_master_send_break(timing_master(ctx));

    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_start_measurement(
        timing_master(ctx), addr, SDI12_MEAS_STANDARD, 0, false, &mresp);

    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "M command failed with error %d", err);
        return r;
    }
    if (mresp.address != addr) {
        TEST_FAIL_MSG(r, "Address echo mismatch: expected '%c', got '%c'",
                      addr, mresp.address);
        return r;
    }
    if (mresp.wait_seconds > 999) {
        TEST_FAIL_MSG(r, "Wait time %d exceeds max 999", mresp.wait_seconds);
        return r;
    }
    if (mresp.value_count > SDI12_M_MAX_VALUES) {
        TEST_FAIL_MSG(r, "Value count %d exceeds max %d",
                      mresp.value_count, SDI12_M_MAX_VALUES);
        return r;
    }

    r.measured_us   = timing_response_latency_us(ctx);
    r.spec_limit_us = SDI12_RESPONSE_TIMEOUT_MS * 1000;

    TEST_PASS_MSG(r, "ttt=%d, n=%d (%.3f ms)",
                  mresp.wait_seconds, mresp.value_count, r.measured_us / 1000.0);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  8. CRC Measurement (aMC!)
 *  §4.4.12 — CRC-16-IBM, 3 ASCII chars appended to D response
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_measurement_crc(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("CRC Measurement (aMC!)", "\xc2\xa7" "4.4.12");

    sdi12_master_send_break(timing_master(ctx));

    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_start_measurement(
        timing_master(ctx), addr, SDI12_MEAS_STANDARD, 0, true, &mresp);

    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "MC command failed: error %d", err);
        return r;
    }

    if (mresp.wait_seconds > 0) {
        sdi12_master_wait_service_request(
            timing_master(ctx), addr,
            (uint32_t)mresp.wait_seconds * 1000 + 1000);
    }

    if (mresp.value_count == 0) {
        TEST_SKIP_MSG(r, "Sensor reports 0 values");
        return r;
    }

    sdi12_data_response_t dresp;
    err = sdi12_master_get_data(timing_master(ctx), addr, 0, true, &dresp);

    if (err == SDI12_ERR_CRC_MISMATCH) {
        TEST_FAIL_MSG(r, "CRC verification failed");
        return r;
    }
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "D0! with CRC failed: error %d", err);
        return r;
    }

    TEST_PASS_MSG(r, "CRC verified, %d values", dresp.value_count);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  9. Concurrent Measurement (aC!)
 *  §4.4.8 — Response: "atttnn\r\n" (nn=0-99)
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_measurement_c(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Concurrent Measurement (aC!)", "\xc2\xa7" "4.4.8");

    sdi12_master_send_break(timing_master(ctx));

    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_start_measurement(
        timing_master(ctx), addr, SDI12_MEAS_CONCURRENT, 0, false, &mresp);

    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "C command failed: error %d", err);
        return r;
    }
    if (mresp.address != addr) {
        TEST_FAIL_MSG(r, "Address echo: expected '%c', got '%c'", addr, mresp.address);
        return r;
    }
    if (mresp.value_count > SDI12_C_MAX_VALUES) {
        TEST_FAIL_MSG(r, "Value count %d exceeds max %d",
                      mresp.value_count, SDI12_C_MAX_VALUES);
        return r;
    }

    r.measured_us   = timing_response_latency_us(ctx);
    r.spec_limit_us = SDI12_RESPONSE_TIMEOUT_MS * 1000;

    TEST_PASS_MSG(r, "ttt=%d, nn=%d (%.3f ms)",
                  mresp.wait_seconds, mresp.value_count, r.measured_us / 1000.0);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  10. Concurrent + CRC (aCC!)
 *  §4.4.8 + §4.4.12
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_measurement_cc(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Concurrent + CRC (aCC!)", "\xc2\xa7" "4.4.8/12");

    sdi12_master_send_break(timing_master(ctx));

    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_start_measurement(
        timing_master(ctx), addr, SDI12_MEAS_CONCURRENT, 0, true, &mresp);

    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "CC command failed: error %d", err);
        return r;
    }

    if (mresp.wait_seconds > 0)
        ctx->hal->delay_ms(ctx->hal, (uint32_t)mresp.wait_seconds * 1000 + 500);

    if (mresp.value_count == 0) {
        TEST_SKIP_MSG(r, "0 values for CC");
        return r;
    }

    sdi12_master_send_break(timing_master(ctx));

    sdi12_data_response_t dresp;
    err = sdi12_master_get_data(timing_master(ctx), addr, 0, true, &dresp);

    if (err == SDI12_ERR_CRC_MISMATCH) {
        TEST_FAIL_MSG(r, "CRC verification failed on CC data");
        return r;
    }
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "D0! with CRC failed: error %d", err);
        return r;
    }

    TEST_PASS_MSG(r, "CRC verified, %d values", dresp.value_count);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  11. Service Request Timing
 *  §4.4.6 — Must arrive within ttt seconds + margin
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_service_request(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Service Request Timing", "\xc2\xa7" "4.4.6");

    sdi12_master_send_break(timing_master(ctx));

    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_start_measurement(
        timing_master(ctx), addr, SDI12_MEAS_STANDARD, 0, false, &mresp);

    if (err != SDI12_OK) {
        TEST_ERROR_MSG(r, "M command failed: error %d", err);
        return r;
    }
    if (mresp.wait_seconds == 0 && mresp.value_count > 0) {
        TEST_SKIP_MSG(r, "ttt=0, sensor provides immediate data");
        return r;
    }
    if (mresp.value_count == 0) {
        TEST_SKIP_MSG(r, "No values to measure");
        return r;
    }

    uint64_t sr_start = ctx->hal->micros(ctx->hal);
    uint32_t timeout  = (uint32_t)mresp.wait_seconds * 1000 + 1000;

    err = sdi12_master_wait_service_request(timing_master(ctx), addr, timeout);

    uint64_t sr_elapsed = ctx->hal->micros(ctx->hal) - sr_start;

    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "No service request within %u ms", timeout);
        return r;
    }

    r.measured_us   = sr_elapsed;
    r.spec_limit_us = ((uint64_t)mresp.wait_seconds + 1) * 1000000ULL;

    if (sr_elapsed > r.spec_limit_us) {
        TEST_FAIL_MSG(r, "Late: %.3f ms (ttt=%ds)", sr_elapsed / 1000.0, mresp.wait_seconds);
        return r;
    }

    TEST_PASS_MSG(r, "Arrived in %.3f ms (ttt=%ds)", sr_elapsed / 1000.0, mresp.wait_seconds);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  12. Data Retrieval (aD0!)
 *  §4.4.7 — Values with mandatory sign prefix
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_data_retrieval(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Data Retrieval (aD0!)", "\xc2\xa7" "4.4.7");

    sdi12_master_send_break(timing_master(ctx));

    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_start_measurement(
        timing_master(ctx), addr, SDI12_MEAS_STANDARD, 0, false, &mresp);

    if (err != SDI12_OK) {
        TEST_ERROR_MSG(r, "M command failed: error %d", err);
        return r;
    }

    if (mresp.wait_seconds > 0) {
        sdi12_master_wait_service_request(
            timing_master(ctx), addr,
            (uint32_t)mresp.wait_seconds * 1000 + 1000);
    }

    if (mresp.value_count == 0) {
        TEST_SKIP_MSG(r, "Sensor reports 0 values");
        return r;
    }

    uint8_t total = 0;
    for (uint8_t page = 0; total < mresp.value_count && page < 10; page++) {
        sdi12_data_response_t dresp;
        err = sdi12_master_get_data(timing_master(ctx), addr, page, false, &dresp);

        if (err != SDI12_OK) {
            TEST_FAIL_MSG(r, "D%d! failed: error %d", page, err);
            return r;
        }
        total += dresp.value_count;
    }

    if (total != mresp.value_count) {
        TEST_FAIL_MSG(r, "Value count mismatch: M said %d, got %d",
                      mresp.value_count, total);
        return r;
    }

    r.measured_us   = timing_response_latency_us(ctx);
    r.spec_limit_us = SDI12_RESPONSE_TIMEOUT_MS * 1000;

    TEST_PASS_MSG(r, "%d values retrieved", total);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  13. Data Retention
 *  §4.4.7 — Data persists until next M command or power loss
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_data_retention(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Data Retention", "\xc2\xa7" "4.4.7");

    sdi12_master_send_break(timing_master(ctx));

    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_start_measurement(
        timing_master(ctx), addr, SDI12_MEAS_STANDARD, 0, false, &mresp);

    if (err != SDI12_OK || mresp.value_count == 0) {
        TEST_SKIP_MSG(r, "No measurement data to test retention");
        return r;
    }

    if (mresp.wait_seconds > 0) {
        sdi12_master_wait_service_request(
            timing_master(ctx), addr,
            (uint32_t)mresp.wait_seconds * 1000 + 1000);
    }

    /* Read data first time */
    sdi12_data_response_t d1;
    err = sdi12_master_get_data(timing_master(ctx), addr, 0, false, &d1);
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "First D0! failed: error %d", err);
        return r;
    }

    /* Read data second time — should still be available */
    sdi12_master_send_break(timing_master(ctx));

    sdi12_data_response_t d2;
    err = sdi12_master_get_data(timing_master(ctx), addr, 0, false, &d2);
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "Second D0! failed: error %d -- data not retained", err);
        return r;
    }
    if (d1.value_count != d2.value_count) {
        TEST_FAIL_MSG(r, "Value count changed: %d -> %d", d1.value_count, d2.value_count);
        return r;
    }

    TEST_PASS_MSG(r, "Data retained across %d values", d1.value_count);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  14. Continuous Measurement (aR0!)
 *  §4.4.9 — Immediate data response, no wait
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_continuous(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Continuous Measurement (aR0!)", "\xc2\xa7" "4.4.9");

    sdi12_master_send_break(timing_master(ctx));

    sdi12_data_response_t dresp;
    sdi12_err_t err = sdi12_master_continuous(timing_master(ctx), addr, 0, false, &dresp);

    if (err == SDI12_ERR_TIMEOUT) {
        TEST_SKIP_MSG(r, "Sensor does not support R0");
        return r;
    }
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "R0 command failed: error %d", err);
        return r;
    }

    r.measured_us   = timing_response_latency_us(ctx);
    r.spec_limit_us = SDI12_RESPONSE_TIMEOUT_MS * 1000;

    TEST_PASS_MSG(r, "%d values in %.3f ms", dresp.value_count, r.measured_us / 1000.0);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  15. Continuous + CRC (aRC0!)
 *  §4.4.9 + §4.4.12
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_continuous_crc(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Continuous + CRC (aRC0!)", "\xc2\xa7" "4.4.9/12");

    sdi12_master_send_break(timing_master(ctx));

    sdi12_data_response_t dresp;
    sdi12_err_t err = sdi12_master_continuous(timing_master(ctx), addr, 0, true, &dresp);

    if (err == SDI12_ERR_TIMEOUT) {
        TEST_SKIP_MSG(r, "Sensor does not support RC0");
        return r;
    }
    if (err == SDI12_ERR_CRC_MISMATCH) {
        TEST_FAIL_MSG(r, "CRC verification failed on RC0 response");
        return r;
    }
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "RC0 command failed: error %d", err);
        return r;
    }

    TEST_PASS_MSG(r, "CRC verified, %d values", dresp.value_count);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  16. Verification (aV!)
 *  §4.4.10 — Same format as M response
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_verify(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Verification (aV!)", "\xc2\xa7" "4.4.10");

    sdi12_master_send_break(timing_master(ctx));

    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_verify(timing_master(ctx), addr, &mresp);

    if (err == SDI12_ERR_TIMEOUT) {
        TEST_SKIP_MSG(r, "Sensor does not support V command");
        return r;
    }
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "V command failed: error %d", err);
        return r;
    }

    TEST_PASS_MSG(r, "ttt=%d, n=%d", mresp.wait_seconds, mresp.value_count);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  17. Address Change (aAb!)
 *  §4.4.4 — Change address and restore
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_address_change(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Address Change (aAb!)", "\xc2\xa7" "4.4.4");

    char temp = (addr == '9') ? '8' : '9';

    sdi12_master_send_break(timing_master(ctx));

    /* Change to temp */
    sdi12_err_t err = sdi12_master_change_address(timing_master(ctx), addr, temp);
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "Change %c->%c failed: error %d", addr, temp, err);
        return r;
    }

    /* Verify at new address */
    sdi12_master_send_break(timing_master(ctx));
    bool present = false;
    sdi12_master_acknowledge(timing_master(ctx), temp, &present);

    if (!present) {
        TEST_FAIL_MSG(r, "Sensor not responding at new address '%c'", temp);
        /* Try to restore */
        sdi12_master_send_break(timing_master(ctx));
        sdi12_master_change_address(timing_master(ctx), temp, addr);
        return r;
    }

    /* Restore original */
    sdi12_master_send_break(timing_master(ctx));
    err = sdi12_master_change_address(timing_master(ctx), temp, addr);

    if (err != SDI12_OK) {
        TEST_ERROR_MSG(r, "RESTORE FAILED %c->%c! Sensor may be at '%c'", temp, addr, temp);
        return r;
    }

    /* Verify restoration */
    sdi12_master_send_break(timing_master(ctx));
    present = false;
    sdi12_master_acknowledge(timing_master(ctx), addr, &present);

    if (!present) {
        TEST_ERROR_MSG(r, "Sensor not responding at restored address '%c'", addr);
        return r;
    }

    TEST_PASS_MSG(r, "Changed %c->%c->%c successfully", addr, temp, addr);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  18. Break Aborts Measurement
 *  §4.2.1 — Break during measurement must reset sensor state
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_break_abort(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Break Aborts Measurement", "\xc2\xa7" "4.2.1");

    sdi12_master_send_break(timing_master(ctx));

    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_start_measurement(
        timing_master(ctx), addr, SDI12_MEAS_STANDARD, 0, false, &mresp);

    if (err != SDI12_OK) {
        TEST_ERROR_MSG(r, "M command failed: error %d", err);
        return r;
    }

    /* Immediately send another break */
    sdi12_master_send_break(timing_master(ctx));

    /* Sensor should respond normally to a new command */
    bool present = false;
    sdi12_master_acknowledge(timing_master(ctx), addr, &present);

    if (!present) {
        TEST_FAIL_MSG(r, "Sensor did not respond after break-abort");
        return r;
    }

    TEST_PASS_MSG(r, "Sensor recovered after break during measurement");
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  19. Extended Command (aX!)
 *  §4.4.11
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_extended(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Extended Command (aX!)", "\xc2\xa7" "4.4.11");

    sdi12_master_send_break(timing_master(ctx));

    char   resp[82];
    size_t resp_len = sizeof(resp);
    sdi12_err_t err = sdi12_master_extended(
        timing_master(ctx), addr, "", resp, &resp_len, 1000);

    if (err == SDI12_ERR_TIMEOUT) {
        TEST_SKIP_MSG(r, "No response to aX! (may not support extended commands)");
        return r;
    }
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "Extended command error: %d", err);
        return r;
    }

    TEST_PASS_MSG(r, "Response received (%zu bytes)", resp_len);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  20. Measurement Groups (aM1! through aM9!)
 *  §4.4.6
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_measurement_groups(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Measurement Groups (aM1-M9)", "\xc2\xa7" "4.4.6");

    int  supported = 0;
    char groups[16];
    memset(groups, 0, sizeof(groups));

    sdi12_master_send_break(timing_master(ctx));

    for (uint8_t g = 1; g <= 9; g++) {
        sdi12_meas_response_t mresp;
        sdi12_err_t err = sdi12_master_start_measurement(
            timing_master(ctx), addr, SDI12_MEAS_STANDARD, g, false, &mresp);

        if (err == SDI12_OK && mresp.value_count > 0)
            groups[supported++] = '0' + g;

        sdi12_master_send_break(timing_master(ctx));
    }

    if (supported == 0) {
        TEST_SKIP_MSG(r, "No additional measurement groups");
        return r;
    }

    groups[supported] = '\0';
    TEST_PASS_MSG(r, "%d group(s): M%s", supported, groups);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  21. Full Bus Scan (all 62 addresses)
 *  §4.4.1
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_bus_scan(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Full Bus Scan (62 addresses)", "\xc2\xa7" "4.4.1");
    (void)addr;

    static const char addrs[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    sdi12_master_send_break(timing_master(ctx));

    int  found = 0;
    char found_addrs[64];
    memset(found_addrs, 0, sizeof(found_addrs));

    for (size_t i = 0; i < sizeof(addrs) - 1; i++) {
        bool present = false;
        sdi12_master_acknowledge(timing_master(ctx), addrs[i], &present);
        if (present)
            found_addrs[found++] = addrs[i];
    }

    if (found == 0) {
        TEST_FAIL_MSG(r, "No sensors found on bus");
        return r;
    }

    found_addrs[found] = '\0';
    TEST_PASS_MSG(r, "Found %d sensor(s): %s", found, found_addrs);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  22. Response Compliance
 *  §4.3 / §4.4 — Correct response to all required commands,
 *                 silence (or error) for unsupported commands
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * Sends a raw command and checks whether the sensor responds or stays
 * silent.  Returns 1 if response, 0 if silence, -1 on error.
 */
static int probe_command(timing_ctx_t *ctx, const char *cmd) {
    sdi12_master_send_break(timing_master(ctx));
    sdi12_err_t err = sdi12_master_transact(timing_master(ctx), cmd, 200);
    if (err == SDI12_ERR_TIMEOUT)
        return 0;    /* silence — expected for unsupported */
    if (err == SDI12_OK)
        return 1;    /* got a response */
    return -1;       /* bus error */
}

static test_result_t test_response_compliance(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Response Compliance", "\xc2\xa7" "4.3/4.4");

    char cmd[16];
    int  failures   = 0;
    int  checks     = 0;
    char fail_detail[256];
    fail_detail[0] = '\0';
    size_t dpos = 0;

    /*
     * Required commands — sensor MUST respond to these.
     * Per §4.4: a!, ?!, aI!, aAb!, aM!, aD0!, aV! are mandatory.
     */
    static const char *required_bodies[] = {
        "!",       /* acknowledge */
        "I!",      /* identify    */
        "M!",      /* measure     */
        "D0!",     /* data page 0 */
        NULL
    };

    for (int i = 0; required_bodies[i]; i++) {
        snprintf(cmd, sizeof(cmd), "%c%s", addr, required_bodies[i]);
        int result = probe_command(ctx, cmd);
        checks++;
        if (result == 0) {
            failures++;
            dpos += (size_t)snprintf(fail_detail + dpos,
                                     sizeof(fail_detail) - dpos,
                                     "%s: no response; ", cmd);
        }
    }

    /* ?! (address query — no address prefix) */
    {
        int result = probe_command(ctx, "?!");
        checks++;
        if (result == 0) {
            failures++;
            dpos += (size_t)snprintf(fail_detail + dpos,
                                     sizeof(fail_detail) - dpos,
                                     "?!: no response; ");
        }
    }

    /*
     * Commands the sensor is NOT expected to support (unless it does).
     * The correct behavior for unsupported commands is silence —
     * any response is acceptable too (sensor may support them),
     * but a malformed response is a failure.
     *
     * We test several optional commands to verify the sensor either
     * responds correctly or stays silent.  We do NOT flag silence as
     * a failure for these.
     */
    static const char *optional_bodies[] = {
        "C!",      /* concurrent      */
        "R0!",     /* continuous       */
        "V!",      /* verify           */
        "MC!",     /* M with CRC       */
        "CC!",     /* C with CRC       */
        "RC0!",    /* R with CRC       */
        "M1!",     /* group 1          */
        "X!",      /* extended         */
        NULL
    };

    int optional_responded = 0;
    int optional_silent    = 0;

    for (int i = 0; optional_bodies[i]; i++) {
        snprintf(cmd, sizeof(cmd), "%c%s", addr, optional_bodies[i]);
        int result = probe_command(ctx, cmd);
        checks++;
        if (result == 1)
            optional_responded++;
        else if (result == 0)
            optional_silent++;
    }

    /*
     * Invalid commands — sensor MUST NOT respond.
     * Per §4.3: a sensor must ignore commands not addressed to it
     * and commands with invalid format.
     */
    char wrong = (addr == '1') ? '2' : '1';
    snprintf(cmd, sizeof(cmd), "%c!", wrong);
    {
        int result = probe_command(ctx, cmd);
        checks++;
        if (result == 1) {
            failures++;
            dpos += (size_t)snprintf(fail_detail + dpos,
                                     sizeof(fail_detail) - dpos,
                                     "%s: unexpected response to wrong addr; ", cmd);
        }
    }

    if (failures > 0) {
        fail_detail[sizeof(fail_detail) - 1] = '\0';
        TEST_FAIL_MSG(r, "%d issue(s) in %d checks: %.200s", failures, checks, fail_detail);
        return r;
    }

    TEST_PASS_MSG(r, "%d checks OK (%d optional responded, %d silent)",
                  checks, optional_responded, optional_silent);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  23. High-Volume ASCII (aHA!)
 *  §4.4.13 — Response: "atttnnn\r\n", up to 999 values via D pages
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * Helper: run a full HV measurement cycle.
 *
 * Sends aHA!/aHAC! → waits for service request → fetches D0..D9+ pages
 * until all values are retrieved.  Validates total value count matches
 * the nnn field from the measurement response.
 *
 * @param ctx     Timing context.
 * @param addr    Sensor address.
 * @param crc     If true, sends aHAC! and verifies CRC on each D page.
 * @param r       Test result to populate.
 * @return true if the test completed (pass or fail), false to skip.
 */
static bool hv_ascii_cycle(timing_ctx_t *ctx, char addr, bool crc,
                           test_result_t *r)
{
    sdi12_master_send_break(timing_master(ctx));

    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_start_measurement(
        timing_master(ctx), addr, SDI12_MEAS_HIGHVOL_ASCII, 0, crc, &mresp);

    if (err == SDI12_ERR_TIMEOUT) {
        TEST_SKIP_MSG(*r, "Sensor does not support aH%s!", crc ? "AC" : "A");
        return false;
    }
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(*r, "H%s command failed: error %d", crc ? "AC" : "A", err);
        return true;
    }
    if (mresp.address != addr) {
        TEST_FAIL_MSG(*r, "Address echo mismatch: expected '%c', got '%c'",
                      addr, mresp.address);
        return true;
    }
    if (mresp.value_count > SDI12_H_MAX_VALUES) {
        TEST_FAIL_MSG(*r, "Value count %d exceeds max %d",
                      mresp.value_count, SDI12_H_MAX_VALUES);
        return true;
    }
    if (mresp.value_count == 0) {
        TEST_SKIP_MSG(*r, "Sensor reports 0 values for H%s", crc ? "AC" : "A");
        return false;
    }

    r->measured_us   = timing_response_latency_us(ctx);
    r->spec_limit_us = SDI12_RESPONSE_TIMEOUT_MS * 1000;

    /* Wait for service request if ttt > 0 */
    if (mresp.wait_seconds > 0) {
        uint32_t sr_timeout = (uint32_t)mresp.wait_seconds * 1000 + 1000;
        err = sdi12_master_wait_service_request(
            timing_master(ctx), addr, sr_timeout);
        if (err != SDI12_OK) {
            TEST_FAIL_MSG(*r, "No service request within %u ms", sr_timeout);
            return true;
        }
    }

    /* Fetch D pages until all values retrieved (up to 10 pages) */
    uint16_t total = 0;
    for (uint8_t page = 0; total < mresp.value_count && page < 10; page++) {
        sdi12_data_response_t dresp;
        err = sdi12_master_get_data(timing_master(ctx), addr, page, crc, &dresp);

        if (err == SDI12_ERR_CRC_MISMATCH) {
            TEST_FAIL_MSG(*r, "CRC mismatch on D%d! page", page);
            return true;
        }
        if (err != SDI12_OK) {
            TEST_FAIL_MSG(*r, "D%d! failed: error %d", page, err);
            return true;
        }
        if (dresp.value_count == 0) {
            /* Empty page — no more data */
            break;
        }
        total += dresp.value_count;
    }

    if (total != mresp.value_count) {
        TEST_FAIL_MSG(*r, "Value count mismatch: H%s said %d, D pages gave %d",
                      crc ? "AC" : "A", mresp.value_count, total);
        return true;
    }

    TEST_PASS_MSG(*r, "ttt=%d, nnn=%d, %d values across D pages%s",
                  mresp.wait_seconds, mresp.value_count, total,
                  crc ? " (CRC OK)" : "");
    return true;
}

static test_result_t test_hv_ascii(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("High-Volume ASCII (aHA!)", "\xc2\xa7" "4.4.13");
    hv_ascii_cycle(ctx, addr, false, &r);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  24. High-Volume ASCII + CRC (aHAC!)
 *  §4.4.13 + §4.4.12 — HV ASCII with CRC-16 on D responses
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_hv_ascii_crc(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("High-Volume ASCII + CRC (aHAC!)", "\xc2\xa7" "4.4.13/12");
    hv_ascii_cycle(ctx, addr, true, &r);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  25 / 31. High-Volume Binary (aHB! / aHBC!)
 *  §5.2 — Binary packet framing: aDBn! retrieves
 *         addr(1) + pkt_size(2 LE) + type(1) + payload(N) + CRC(2 LE)
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * Helper: run a full HV binary measurement cycle.
 *
 * Sends aHB!/aHBC! → waits for service request → fetches binary data
 * pages via aDBn! per §5.2.  Each binary packet is verified for:
 *   - 16-bit CRC (always present in binary packets)
 *   - Valid binary type code
 *   - Payload alignment to element size
 *
 * @param ctx   Timing context.
 * @param addr  Sensor address.
 * @param crc   If true, sends aHBC! instead of aHB!.
 * @param r     Test result to populate.
 * @return true if the test completed (pass or fail), false to skip.
 */
static bool hv_binary_cycle(timing_ctx_t *ctx, char addr, bool crc,
                            test_result_t *r)
{
    sdi12_master_send_break(timing_master(ctx));

    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_start_measurement(
        timing_master(ctx), addr, SDI12_MEAS_HIGHVOL_BINARY, 0, crc, &mresp);

    if (err == SDI12_ERR_TIMEOUT) {
        TEST_SKIP_MSG(*r, "Sensor does not support aHB%s!", crc ? "C" : "");
        return false;
    }
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(*r, "HB%s command failed: error %d", crc ? "C" : "", err);
        return true;
    }
    if (mresp.address != addr) {
        TEST_FAIL_MSG(*r, "Address echo mismatch: expected '%c', got '%c'",
                      addr, mresp.address);
        return true;
    }
    if (mresp.value_count > SDI12_H_MAX_VALUES) {
        TEST_FAIL_MSG(*r, "Value count %d exceeds max %d",
                      mresp.value_count, SDI12_H_MAX_VALUES);
        return true;
    }
    if (mresp.value_count == 0) {
        TEST_SKIP_MSG(*r, "Sensor reports 0 values for HB%s", crc ? "C" : "");
        return false;
    }

    r->measured_us   = timing_response_latency_us(ctx);
    r->spec_limit_us = SDI12_RESPONSE_TIMEOUT_MS * 1000;

    /* Wait for service request if ttt > 0 */
    if (mresp.wait_seconds > 0) {
        uint32_t sr_timeout = (uint32_t)mresp.wait_seconds * 1000 + 1000;
        err = sdi12_master_wait_service_request(
            timing_master(ctx), addr, sr_timeout);
        if (err != SDI12_OK) {
            TEST_FAIL_MSG(*r, "No service request within %u ms", sr_timeout);
            return true;
        }
    }

    /* Enable binary recv mode — aDBn! packets may contain 0x0A bytes */
    timing_set_binary_recv(ctx, true);

    /* Fetch binary data pages via aDBn! (§5.2 binary packet framing).
     * Each packet: addr(1) + pkt_size(2 LE) + type(1) + payload(N) + CRC(2 LE).
     * CRC is always present and verified by the master API. */
    uint16_t total = 0;
    for (uint16_t page = 0; total < mresp.value_count && page < SDI12_MAX_HV_DATA_PAGES; page++) {
        sdi12_bintype_t btype;
        uint8_t payload[SDI12_BIN_MAX_PAYLOAD];
        size_t payload_len = sizeof(payload);

        err = sdi12_master_get_hv_binary_data(
            timing_master(ctx), addr, page, &btype, payload, &payload_len);

        if (err == SDI12_ERR_CRC_MISMATCH) {
            timing_set_binary_recv(ctx, false);
            TEST_FAIL_MSG(*r, "CRC mismatch on DB%u! binary packet", page);
            return true;
        }
        if (err != SDI12_OK) {
            timing_set_binary_recv(ctx, false);
            TEST_FAIL_MSG(*r, "DB%u! failed: error %d", page, err);
            return true;
        }
        if (payload_len == 0) break;  /* empty packet — no more data */

        size_t elem_sz = sdi12_bintype_size(btype);
        if (elem_sz == 0) {
            timing_set_binary_recv(ctx, false);
            TEST_FAIL_MSG(*r, "DB%u! invalid binary type code 0x%02X",
                          page, (unsigned)btype);
            return true;
        }

        if (payload_len % elem_sz != 0) {
            timing_set_binary_recv(ctx, false);
            TEST_FAIL_MSG(*r, "DB%u! payload %zu bytes not a multiple of %zu "
                          "(type %d)", page, payload_len, elem_sz, btype);
            return true;
        }

        total += (uint16_t)(payload_len / elem_sz);
    }

    timing_set_binary_recv(ctx, false);

    if (total != mresp.value_count) {
        TEST_FAIL_MSG(*r, "Value count mismatch: HB%s said %d, binary pages gave %d",
                      crc ? "C" : "", mresp.value_count, total);
        return true;
    }

    TEST_PASS_MSG(*r, "ttt=%d, nnn=%d, %d binary values (aDBn! CRC OK)%s",
                  mresp.wait_seconds, mresp.value_count, total,
                  crc ? " [HBC]" : "");
    return true;
}

static test_result_t test_hv_binary(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("High-Volume Binary (aHB!)", "\xc2\xa7" "5.2");
    hv_binary_cycle(ctx, addr, false, &r);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  26. Identify Measurement Metadata (aIM!, aIM_nnn!)
 *  §4.4.15 — Probe measurement metadata and per-parameter SHEF/units
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_identify_measurement(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Identify Measurement (aIM!)", "\xc2\xa7" "4.4.15");

    sdi12_master_send_break(timing_master(ctx));

    /* aIM! → atttn — describes what aM! returns */
    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_identify_measurement(
        timing_master(ctx), addr, "M", SDI12_MEAS_STANDARD, &mresp);

    if (err == SDI12_ERR_TIMEOUT) {
        TEST_SKIP_MSG(r, "Sensor does not support aIM!");
        return r;
    }
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "aIM! failed: error %d", err);
        return r;
    }
    if (mresp.address != addr) {
        TEST_FAIL_MSG(r, "Address echo mismatch: expected '%c', got '%c'",
                      addr, mresp.address);
        return r;
    }
    if (mresp.value_count > SDI12_M_MAX_VALUES) {
        TEST_FAIL_MSG(r, "IM value count %d exceeds M max %d",
                      mresp.value_count, SDI12_M_MAX_VALUES);
        return r;
    }

    r.measured_us   = timing_response_latency_us(ctx);
    r.spec_limit_us = SDI12_RESPONSE_TIMEOUT_MS * 1000;

    uint16_t n = mresp.value_count;
    if (n == 0) {
        TEST_PASS_MSG(r, "aIM! responded: n=0 (no M parameters)");
        return r;
    }

    /* Probe per-parameter metadata aIM_001! .. aIM_nnn! */
    uint16_t meta_ok = 0;
    char meta_summary[256];
    size_t spos = 0;

    for (uint16_t p = 1; p <= n && p <= 9; p++) {
        sdi12_param_meta_response_t pmeta;
        err = sdi12_master_identify_param(
            timing_master(ctx), addr, "M", p, &pmeta);

        if (err == SDI12_ERR_TIMEOUT) continue; /* optional per spec */
        if (err != SDI12_OK) continue;

        if (pmeta.shef[0] != '\0') {
            meta_ok++;
            if (spos > 0 && spos < sizeof(meta_summary) - 2)
                spos += (size_t)snprintf(meta_summary + spos,
                         sizeof(meta_summary) - spos, ", ");
            spos += (size_t)snprintf(meta_summary + spos,
                     sizeof(meta_summary) - spos, "%s(%s)",
                     pmeta.shef, pmeta.units);
        }
    }

    if (meta_ok > 0) {
        TEST_PASS_MSG(r, "n=%d, %d params with metadata: %s",
                      n, meta_ok, meta_summary);
    } else {
        TEST_PASS_MSG(r, "n=%d (per-param metadata not provided)", n);
    }
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  27. Extended Multi-line Response (aX!)
 *  §4.4.11 — Extended commands may return multi-line responses
 *            separated by up to 150ms inter-line gaps
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_extended_multiline(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Extended Multi-Line (aX!)", "\xc2\xa7" "4.4.11");

    sdi12_master_send_break(timing_master(ctx));

    /* Try a bare aX! to see if the sensor responds at all */
    char   resp[820];
    size_t resp_len = 0;
    uint8_t lines = 0;

    sdi12_err_t err = sdi12_master_extended_multiline(
        timing_master(ctx), addr, "", resp, sizeof(resp),
        &resp_len, &lines, 1000);

    if (err == SDI12_ERR_TIMEOUT) {
        TEST_SKIP_MSG(r, "No response to aX! (extended commands not supported)");
        return r;
    }
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "Extended command error: %d", err);
        return r;
    }

    r.measured_us   = timing_response_latency_us(ctx);
    r.spec_limit_us = SDI12_RESPONSE_TIMEOUT_MS * 1000;

    if (lines <= 1) {
        TEST_PASS_MSG(r, "Single-line response (%zu bytes, %d line)",
                      resp_len, lines);
        return r;
    }

    /* Multi-line response — check inter-line timing */
    uint64_t max_gap = timing_max_interline_gap_us(ctx);
    uint64_t gap_limit_us = SDI12_MULTILINE_GAP_MS * 1000;

    if (max_gap > gap_limit_us) {
        TEST_FAIL_MSG(r, "%d lines, max inter-line gap %.1f ms exceeds "
                      "%.1f ms limit",
                      lines, max_gap / 1000.0, gap_limit_us / 1000.0);
        return r;
    }

    TEST_PASS_MSG(r, "%d lines, %zu bytes, max gap %.1f ms (limit: %.1f ms)",
                  lines, resp_len, max_gap / 1000.0, gap_limit_us / 1000.0);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  28. Identify Version Field (ll)
 *  §4.4.3 — The two-character SDI-12 version must be "13" or "14"
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_identify_version(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Identify Version Field (ll)", "\xc2\xa7" "4.4.3");

    sdi12_master_send_break(timing_master(ctx));

    /* Use raw transact to access the version field directly */
    char cmd[8];
    snprintf(cmd, sizeof(cmd), "%cI!", addr);
    sdi12_err_t err = sdi12_master_transact(timing_master(ctx), cmd,
                                             SDI12_RESPONSE_TIMEOUT_MS);
    if (err == SDI12_ERR_TIMEOUT) {
        TEST_ERROR_MSG(r, "No response to aI!");
        return r;
    }
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "Identify failed: error %d", err);
        return r;
    }

    /* Response: a  ll  cccccccc mmmmmm vvv [serial] \r\n
     *           0  1-2  3-10   11-16  17-19          */
    size_t len = timing_master(ctx)->resp_len;
    if (len < 20) {
        TEST_FAIL_MSG(r, "Response too short (%zu chars)", len);
        return r;
    }

    char v0 = timing_master(ctx)->resp_buf[1];
    char v1 = timing_master(ctx)->resp_buf[2];

    r.measured_us   = timing_response_latency_us(ctx);
    r.spec_limit_us = SDI12_RESPONSE_TIMEOUT_MS * 1000;

    if ((v0 == '1' && v1 == '4') || (v0 == '1' && v1 == '3')) {
        TEST_PASS_MSG(r, "SDI-12 version: %c%c", v0, v1);
    } else {
        TEST_FAIL_MSG(r, "Unexpected version field: '%c%c' (expected 13 or 14)",
                      v0, v1);
    }
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  29. Concurrent Measurement Groups (aC1! through aC9!)
 *  §4.4.8
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_concurrent_groups(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Concurrent Groups (aC1-C9)", "\xc2\xa7" "4.4.8");

    int  supported = 0;
    char groups[16];
    memset(groups, 0, sizeof(groups));

    sdi12_master_send_break(timing_master(ctx));

    for (uint8_t g = 1; g <= 9; g++) {
        sdi12_meas_response_t mresp;
        sdi12_err_t err = sdi12_master_start_measurement(
            timing_master(ctx), addr, SDI12_MEAS_CONCURRENT, g, false, &mresp);

        if (err == SDI12_OK && mresp.value_count > 0)
            groups[supported++] = '0' + g;

        sdi12_master_send_break(timing_master(ctx));
    }

    if (supported == 0) {
        TEST_SKIP_MSG(r, "No additional concurrent groups");
        return r;
    }

    groups[supported] = '\0';
    TEST_PASS_MSG(r, "%d group(s): C%s", supported, groups);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  30. Continuous Measurement Groups (aR1! through aR9!)
 *  §4.4.9
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_continuous_groups(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Continuous Groups (aR1-R9)", "\xc2\xa7" "4.4.9");

    int  supported = 0;
    char groups[16];
    memset(groups, 0, sizeof(groups));

    sdi12_master_send_break(timing_master(ctx));

    for (uint8_t g = 1; g <= 9; g++) {
        sdi12_data_response_t dresp;
        sdi12_err_t err = sdi12_master_continuous(
            timing_master(ctx), addr, g, false, &dresp);

        if (err == SDI12_OK && dresp.value_count > 0)
            groups[supported++] = '0' + g;

        sdi12_master_send_break(timing_master(ctx));
    }

    if (supported == 0) {
        TEST_SKIP_MSG(r, "No additional continuous groups");
        return r;
    }

    groups[supported] = '\0';
    TEST_PASS_MSG(r, "%d group(s): R%s", supported, groups);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  31. High-Volume Binary + CRC (aHBC!)
 *  §5.2 — Binary data via aDBn! (CRC is always present in binary packets)
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_hv_binary_crc(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("High-Volume Binary + CRC (aHBC!)", "\xc2\xa7" "5.2");
    hv_binary_cycle(ctx, addr, true, &r);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Registration
 * ══════════════════════════════════════════════════════════════════════ */

void sensor_tests_register(test_suite_t *suite) {
    test_suite_add(suite, "acknowledge",    "Acknowledge (a!)",                "\xc2\xa7" "4.4.2",    test_acknowledge);
    test_suite_add(suite, "query_address",  "Query Address (?!)",              "\xc2\xa7" "4.4.1",    test_query_address);
    test_suite_add(suite, "identify",       "Identify (aI!)",                  "\xc2\xa7" "4.4.3",    test_identify);
    test_suite_add(suite, "identify_ver",   "Identify Version Field (ll)",     "\xc2\xa7" "4.4.3",    test_identify_version);
    test_suite_add(suite, "wrong_addr",     "Wrong Address Silence",           "\xc2\xa7" "4.3",      test_wrong_address_silence);
    test_suite_add(suite, "response_time",  "Response Timing (<=15ms)",        "\xc2\xa7" "4.2.3",    test_response_timing);
    test_suite_add(suite, "interchar_gap",  "Inter-Character Gap (<=1.66ms)",  "\xc2\xa7" "4.2.4",    test_interchar_gap);
    test_suite_add(suite, "meas_m",         "Standard Measurement (aM!)",      "\xc2\xa7" "4.4.6",    test_measurement_m);
    test_suite_add(suite, "meas_mc",        "CRC Measurement (aMC!)",          "\xc2\xa7" "4.4.12",   test_measurement_crc);
    test_suite_add(suite, "meas_c",         "Concurrent Measurement (aC!)",    "\xc2\xa7" "4.4.8",    test_measurement_c);
    test_suite_add(suite, "meas_cc",        "Concurrent + CRC (aCC!)",         "\xc2\xa7" "4.4.8/12", test_measurement_cc);
    test_suite_add(suite, "service_req",    "Service Request Timing",          "\xc2\xa7" "4.4.6",    test_service_request);
    test_suite_add(suite, "data_d0",        "Data Retrieval (aD0!)",           "\xc2\xa7" "4.4.7",    test_data_retrieval);
    test_suite_add(suite, "data_retain",    "Data Retention",                  "\xc2\xa7" "4.4.7",    test_data_retention);
    test_suite_add(suite, "continuous",     "Continuous Measurement (aR0!)",   "\xc2\xa7" "4.4.9",    test_continuous);
    test_suite_add(suite, "continuous_crc", "Continuous + CRC (aRC0!)",        "\xc2\xa7" "4.4.9/12", test_continuous_crc);
    test_suite_add(suite, "verify",         "Verification (aV!)",              "\xc2\xa7" "4.4.10",   test_verify);
    test_suite_add(suite, "addr_change",    "Address Change (aAb!)",           "\xc2\xa7" "4.4.4",    test_address_change);
    test_suite_add(suite, "break_abort",    "Break Aborts Measurement",        "\xc2\xa7" "4.2.1",    test_break_abort);
    test_suite_add(suite, "extended",       "Extended Command (aX!)",          "\xc2\xa7" "4.4.11",   test_extended);
    test_suite_add(suite, "extended_ml",    "Extended Multi-Line (aX!)",       "\xc2\xa7" "4.4.11",   test_extended_multiline);
    test_suite_add(suite, "meas_groups",    "Measurement Groups (aM1-M9)",     "\xc2\xa7" "4.4.6",    test_measurement_groups);
    test_suite_add(suite, "conc_groups",    "Concurrent Groups (aC1-C9)",      "\xc2\xa7" "4.4.8",    test_concurrent_groups);
    test_suite_add(suite, "cont_groups",    "Continuous Groups (aR1-R9)",      "\xc2\xa7" "4.4.9",    test_continuous_groups);
    test_suite_add(suite, "bus_scan",       "Full Bus Scan (62 addresses)",    "\xc2\xa7" "4.4.1",    test_bus_scan);
    test_suite_add(suite, "resp_compliance","Response Compliance",             "\xc2\xa7" "4.3/4.4",  test_response_compliance);
    test_suite_add(suite, "hv_ascii",       "High-Volume ASCII (aHA!)",        "\xc2\xa7" "4.4.13",   test_hv_ascii);
    test_suite_add(suite, "hv_ascii_crc",   "High-Volume ASCII+CRC (aHAC!)",   "\xc2\xa7" "4.4.13/12",test_hv_ascii_crc);
    test_suite_add(suite, "hv_binary",      "High-Volume Binary (aHB!)",       "\xc2\xa7" "5.2",      test_hv_binary);
    test_suite_add(suite, "hv_binary_crc",  "High-Volume Binary+CRC (aHBC!)",  "\xc2\xa7" "5.2",      test_hv_binary_crc);
    test_suite_add(suite, "identify_meas",  "Identify Measurement (aIM!)",     "\xc2\xa7" "4.4.15",   test_identify_measurement);
}
