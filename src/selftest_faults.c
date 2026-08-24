/**
 * @file selftest_faults.c
 * @brief Fault-injection self-tests — loopback only.
 *
 * The plain loopback self-test runs libsdi12 on both ends of the wire, so
 * a bug that is symmetric in the library cancels out and stays invisible.
 * These tests deliberately break that symmetry via hal_loopback fault
 * modes (see hal_loopback.h): hostile measurement values, a foreign
 * sensor answering binary pages, an undersized master buffer, and a
 * binary formatter written to the documented callback contract.
 *
 * They are registered only in --self-test mode: they exercise the
 * libsdi12 library through the simulated sensor, not an external device.
 */
#include "test_suite.h"
#include "hal_loopback.h"
#include "sdi12_master.h"
#include <string.h>
#include <math.h>

#define SECT_SELFTEST "self-test"

/* ── pd.d value grammar (§4.4.8 Table 11) ─────────────────────────────
 * <values> = pd.d — polarity sign, 1–7 digits, optional single decimal
 * point, max 9 characters total. Anything else (letters, second sign,
 * empty digit run) is malformed on the wire. */
static bool valid_pdd_value(const char *v, size_t len, char *why, size_t why_sz)
{
    if (len < 2) {
        snprintf(why, why_sz, "too short (%zu chars)", len);
        return false;
    }
    if (len > 9) {
        snprintf(why, why_sz, "%zu chars exceeds 9-char max", len);
        return false;
    }
    if (v[0] != '+' && v[0] != '-') {
        snprintf(why, why_sz, "missing polarity sign");
        return false;
    }
    int digits = 0, points = 0;
    for (size_t i = 1; i < len; i++) {
        if (v[i] >= '0' && v[i] <= '9') { digits++; continue; }
        if (v[i] == '.') { points++; continue; }
        snprintf(why, why_sz, "illegal char '%c'", v[i]);
        return false;
    }
    if (points > 1) {
        snprintf(why, why_sz, "multiple decimal points");
        return false;
    }
    if (digits < 1) {
        snprintf(why, why_sz, "no digits");
        return false;
    }
    if (digits > 7) {
        snprintf(why, why_sz, "%d digits exceeds 7-digit max", digits);
        return false;
    }
    return true;
}

/* Validate every value in one raw aDn! response (addr already checked).
 * Returns number of values, or -1 with `why` set on grammar violation. */
static int validate_d_response(const char *resp, size_t len,
                               char *why, size_t why_sz,
                               char *bad, size_t bad_sz)
{
    /* Strip trailing CR/LF */
    while (len > 0 && (resp[len - 1] == '\r' || resp[len - 1] == '\n'))
        len--;

    int count = 0;
    size_t i = 1;  /* skip address char */
    while (i < len) {
        if (resp[i] != '+' && resp[i] != '-') {
            snprintf(why, why_sz, "value does not start with sign");
            snprintf(bad, bad_sz, "%.12s", resp + i);
            return -1;
        }
        size_t start = i;
        i++;  /* consume sign */
        while (i < len && resp[i] != '+' && resp[i] != '-')
            i++;
        size_t vlen = i - start;
        char reason[64];
        if (!valid_pdd_value(resp + start, vlen, reason, sizeof(reason))) {
            snprintf(why, why_sz, "%s", reason);
            snprintf(bad, bad_sz, "%.*s", (int)(vlen > 12 ? 12 : vlen),
                     resp + start);
            return -1;
        }
        count++;
    }
    return count;
}

/* ══════════════════════════════════════════════════════════════════════
 *  F1. Hostile values must still format as valid pd.d
 *  Exposes: format_value() UB / NaN / inf / double-sign emission
 * ══════════════════════════════════════════════════════════════════════ */
static test_result_t test_fault_hostile_values(timing_ctx_t *ctx, char addr)
{
    test_result_t r = TEST_RESULT("Hostile Values Format (NaN/inf/-0.0)",
                                  SECT_SELFTEST);
    hal_loopback_set_faults(ctx->hal, LB_FAULT_HOSTILE_VALUES);

    sdi12_master_send_break(timing_master(ctx));

    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_start_measurement(
        timing_master(ctx), addr, SDI12_MEAS_STANDARD, 0, false, &mresp);
    if (err != SDI12_OK) {
        hal_loopback_set_faults(ctx->hal, LB_FAULT_NONE);
        TEST_FAIL_MSG(r, "M command failed with error %d", err);
        return r;
    }

    int total = 0;
    for (uint8_t page = 0; page <= 9; page++) {
        char cmd[8];
        snprintf(cmd, sizeof(cmd), "%cD%u!", addr, page);
        err = sdi12_master_transact(timing_master(ctx), cmd,
                                    SDI12_RESPONSE_TIMEOUT_MS);
        if (err != SDI12_OK) {
            hal_loopback_set_faults(ctx->hal, LB_FAULT_NONE);
            TEST_FAIL_MSG(r, "D%u! failed with error %d", page, err);
            return r;
        }

        sdi12_master_ctx_t *m = timing_master(ctx);
        if (m->resp_len <= 3)   /* bare "a\r\n" — no more data */
            break;

        char why[96], bad[16];
        int n = validate_d_response(m->resp_buf, m->resp_len,
                                    why, sizeof(why), bad, sizeof(bad));
        if (n < 0) {
            hal_loopback_set_faults(ctx->hal, LB_FAULT_NONE);
            TEST_FAIL_MSG(r, "D%u! emitted malformed value \"%s\": %s "
                          "(non-finite inputs must not corrupt the wire)",
                          page, bad, why);
            return r;
        }
        total += n;
    }

    hal_loopback_set_faults(ctx->hal, LB_FAULT_NONE);

    if (total != mresp.value_count) {
        TEST_FAIL_MSG(r, "Value count mismatch: M promised %d, D pages "
                      "delivered %d", mresp.value_count, total);
        return r;
    }

    TEST_PASS_MSG(r, "%d hostile values all formatted as valid pd.d", total);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  F2. Undersized payload buffer must not report phantom bytes
 *  Exposes: sdi12_master_get_hv_binary_data() out_len > buffer size
 * ══════════════════════════════════════════════════════════════════════ */
static test_result_t test_fault_bin_truncation(timing_ctx_t *ctx, char addr)
{
    test_result_t r = TEST_RESULT("Binary Buffer Truncation Contract",
                                  SECT_SELFTEST);

    sdi12_master_send_break(timing_master(ctx));

    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_start_measurement(
        timing_master(ctx), addr, SDI12_MEAS_HIGHVOL_BINARY, 0, false, &mresp);
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "HB command failed with error %d", err);
        return r;
    }

    timing_set_binary_recv(ctx, true);

    sdi12_bintype_t btype;
    uint8_t small[8];
    size_t out_len = sizeof(small);
    err = sdi12_master_get_hv_binary_data(
        timing_master(ctx), addr, 0, &btype, small, &out_len);

    timing_set_binary_recv(ctx, false);

    if (err == SDI12_OK && out_len > sizeof(small)) {
        TEST_FAIL_MSG(r, "DB0! returned OK with out_len=%zu for an %zu-byte "
                      "buffer — caller following the in/out contract reads "
                      "%zu bytes past its buffer",
                      out_len, sizeof(small), out_len - sizeof(small));
        return r;
    }
    if (err != SDI12_OK && err != SDI12_ERR_BUFFER_OVERFLOW) {
        TEST_FAIL_MSG(r, "DB0! failed with unexpected error %d", err);
        return r;
    }
    if (out_len > sizeof(small)) {
        TEST_FAIL_MSG(r, "out_len=%zu exceeds buffer size %zu",
                      out_len, sizeof(small));
        return r;
    }

    TEST_PASS_MSG(r, "Truncation reported honestly: err=%d, out_len=%zu "
                  "(buffer %zu)", err, out_len, sizeof(small));
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  F3. Binary packet from a foreign address must be rejected
 *  Exposes: sdi12_master_get_hv_binary_data() missing address check
 * ══════════════════════════════════════════════════════════════════════ */
static test_result_t test_fault_bin_foreign_addr(timing_ctx_t *ctx, char addr)
{
    test_result_t r = TEST_RESULT("Foreign-Address Binary Packet Rejected",
                                  SECT_SELFTEST);
    hal_loopback_set_faults(ctx->hal, LB_FAULT_FOREIGN_BIN_ADDR);

    sdi12_master_send_break(timing_master(ctx));

    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_start_measurement(
        timing_master(ctx), addr, SDI12_MEAS_HIGHVOL_BINARY, 0, false, &mresp);
    if (err != SDI12_OK) {
        hal_loopback_set_faults(ctx->hal, LB_FAULT_NONE);
        TEST_FAIL_MSG(r, "HB command failed with error %d", err);
        return r;
    }

    timing_set_binary_recv(ctx, true);

    sdi12_bintype_t btype;
    uint8_t payload[SDI12_BIN_MAX_PAYLOAD];
    size_t out_len = sizeof(payload);
    err = sdi12_master_get_hv_binary_data(
        timing_master(ctx), addr, 0, &btype, payload, &out_len);

    timing_set_binary_recv(ctx, false);
    hal_loopback_set_faults(ctx->hal, LB_FAULT_NONE);

    if (err == SDI12_OK) {
        TEST_FAIL_MSG(r, "DB0! accepted a CRC-valid packet addressed from "
                      "another sensor (%zu payload bytes) — data attributed "
                      "to the wrong device", out_len);
        return r;
    }

    TEST_PASS_MSG(r, "Foreign packet rejected with error %d", err);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  F4. Binary formatter per documented contract must frame correctly
 *  Exposes: format_binary_page doc/implementation contract mismatch
 * ══════════════════════════════════════════════════════════════════════ */
static test_result_t test_fault_bin_doc_contract(timing_ctx_t *ctx, char addr)
{
    test_result_t r = TEST_RESULT("Binary Formatter Documented Contract",
                                  SECT_SELFTEST);
    hal_loopback_set_faults(ctx->hal, LB_FAULT_DOC_CONTRACT_BIN);

    sdi12_master_send_break(timing_master(ctx));

    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_start_measurement(
        timing_master(ctx), addr, SDI12_MEAS_HIGHVOL_BINARY, 0, false, &mresp);
    if (err != SDI12_OK) {
        hal_loopback_set_faults(ctx->hal, LB_FAULT_NONE);
        TEST_FAIL_MSG(r, "HB command failed with error %d", err);
        return r;
    }

    timing_set_binary_recv(ctx, true);

    sdi12_bintype_t btype;
    uint8_t payload[SDI12_BIN_MAX_PAYLOAD];
    size_t out_len = sizeof(payload);
    err = sdi12_master_get_hv_binary_data(
        timing_master(ctx), addr, 0, &btype, payload, &out_len);

    timing_set_binary_recv(ctx, false);
    hal_loopback_set_faults(ctx->hal, LB_FAULT_NONE);

    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "DB0! failed with error %d — a formatter written "
                      "to the sdi12_sensor.h contract mis-framed the packet",
                      err);
        return r;
    }

    size_t elem_sz = sdi12_bintype_size(btype);
    if (elem_sz == 0) {
        TEST_FAIL_MSG(r, "Packet declared invalid type 0x%02X — payload "
                      "byte consumed as the type field", (unsigned)btype);
        return r;
    }
    if (out_len % elem_sz != 0) {
        TEST_FAIL_MSG(r, "Payload %zu bytes not a multiple of element "
                      "size %zu (type %d) — size field off by one",
                      out_len, elem_sz, (int)btype);
        return r;
    }
    if (out_len / elem_sz != mresp.value_count) {
        TEST_FAIL_MSG(r, "HB promised %d values, packet framed %zu",
                      mresp.value_count, out_len / elem_sz);
        return r;
    }

    TEST_PASS_MSG(r, "Doc-contract formatter framed %d values correctly "
                  "(type %d)", mresp.value_count, (int)btype);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════ */

void selftest_fault_tests_register(test_suite_t *suite)
{
    test_suite_add(suite, "fault_values",
                   "Hostile Values Format (NaN/inf/-0.0)", SECT_SELFTEST,
                   test_fault_hostile_values);
    test_suite_add(suite, "fault_bin_trunc",
                   "Binary Buffer Truncation Contract", SECT_SELFTEST,
                   test_fault_bin_truncation);
    test_suite_add(suite, "fault_bin_addr",
                   "Foreign-Address Binary Packet Rejected", SECT_SELFTEST,
                   test_fault_bin_foreign_addr);
    test_suite_add(suite, "fault_bin_doc",
                   "Binary Formatter Documented Contract", SECT_SELFTEST,
                   test_fault_bin_doc_contract);
}
