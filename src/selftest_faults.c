/**
 * @file selftest_faults.c
 * @brief Fault-injection self-tests — loopback only.
 *
 * The plain loopback self-test runs libsdi12 on both ends of the wire, so
 * a bug that is symmetric in the library cancels out and stays invisible.
 * These tests deliberately break that symmetry via hal_loopback fault
 * modes (see hal_loopback.h): hostile measurement values, a foreign
 * sensor answering binary pages, an undersized master buffer, paced
 * line-rate delivery, and boundary/overwide value sets.
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
 * empty digit run) is malformed on the wire.
 * Exported (sdi12v_*) so the live compliance battery can use the same
 * validators against real sensors, not just the loopback self-test. */
bool sdi12v_valid_pdd(const char *v, size_t len, char *why, size_t why_sz)
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
    if (v[len - 1] == '.') {
        /* Table 11: digits follow the decimal point — a bare trailing
         * point is the signature of a truncated value. */
        snprintf(why, why_sz, "trailing decimal point");
        return false;
    }
    return true;
}

/* Validate every value in one raw aDn! response (addr already checked).
 * Returns number of values, or -1 with `why` set on grammar violation. */
int sdi12v_validate_values(const char *resp, size_t len,
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
        if (!sdi12v_valid_pdd(resp + start, vlen, reason, sizeof(reason))) {
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
        int n = sdi12v_validate_values(m->resp_buf, m->resp_len,
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
 *  F5. Line-rate response delivery must not truncate reads
 *  Exposes: recv layer treating its timeout as a total-read deadline
 * ══════════════════════════════════════════════════════════════════════ */
static test_result_t test_fault_paced_uart(timing_ctx_t *ctx, char addr)
{
    test_result_t r = TEST_RESULT("Paced-UART Response Delivery",
                                  SECT_SELFTEST);
    hal_loopback_set_faults(ctx->hal, LB_FAULT_PACED_UART);

    /* aI! has the longest fixed-format response (>= 20 chars). Paced at
     * ~2 ms/byte it takes ~50 ms of line time — like a real 1200-baud
     * bus, where 8.33 ms/char puts even "a<CR><LF>" past a 15 ms total
     * deadline. The 15 ms response timeout is time-to-FIRST-byte
     * (spec 4.0); a recv that stops reading at that deadline truncates
     * every response. */
    sdi12_master_send_break(timing_master(ctx));

    sdi12_ident_t ident;
    memset(&ident, 0, sizeof(ident));
    sdi12_err_t err = sdi12_master_identify(timing_master(ctx), addr, &ident);

    hal_loopback_set_faults(ctx->hal, LB_FAULT_NONE);

    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "aI! failed with error %d at line-rate delivery — "
                      "recv deadline cuts the response short", err);
        return r;
    }
    if (memcmp(ident.vendor, "LOOPBAK ", 8) != 0) {
        TEST_FAIL_MSG(r, "aI! returned corrupted identity \"%.8s\" — "
                      "response truncated mid-read", ident.vendor);
        return r;
    }

    TEST_PASS_MSG(r, "Full aI! response received at line-rate delivery");
    return r;
}

/* Shared: run aHA! + fetch all aDn! pages, validating grammar and the
 * 75-char <values> limit; return delivered count (or -1, r set FAILed). */
static int ha_fetch_all(timing_ctx_t *ctx, char addr,
                        sdi12_meas_response_t *mresp, test_result_t *r)
{
    sdi12_master_send_break(timing_master(ctx));

    sdi12_err_t err = sdi12_master_start_measurement(
        timing_master(ctx), addr, SDI12_MEAS_HIGHVOL_ASCII, 0, false, mresp);
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(*r, "HA command failed with error %d", err);
        return -1;
    }

    int total = 0;
    for (uint16_t page = 0; page < 1000; page++) {
        char cmd[8];
        snprintf(cmd, sizeof(cmd), "%cD%u!", addr, page);
        err = sdi12_master_transact(timing_master(ctx), cmd,
                                    SDI12_RESPONSE_TIMEOUT_MS);
        if (err != SDI12_OK) {
            TEST_FAIL_MSG(*r, "D%u! failed with error %d", page, err);
            return -1;
        }

        sdi12_master_ctx_t *m = timing_master(ctx);
        size_t len = m->resp_len;
        while (len > 0 && (m->resp_buf[len - 1] == '\r' ||
                           m->resp_buf[len - 1] == '\n'))
            len--;
        if (len <= 1)
            break;                       /* bare address — no more data */
        if (!sdi12_crc_verify(m->resp_buf, m->resp_len)) {
            TEST_FAIL_MSG(*r, "D%u! page lacks the mandatory CRC", page);
            return -1;
        }
        size_t vals_len = len - 1 - 3;   /* minus address and CRC */
        if (vals_len > 75) {
            TEST_FAIL_MSG(*r, "D%u! <values> field is %zu chars — "
                          "\xc2\xa7" "4.4.8 caps it at 75", page, vals_len);
            return -1;
        }

        char why[96], bad[16];
        int n = sdi12v_validate_values(m->resp_buf, len - 3,
                                       why, sizeof(why), bad, sizeof(bad));
        if (n < 0) {
            TEST_FAIL_MSG(*r, "D%u! emitted malformed value \"%s\": %s",
                          page, bad, why);
            return -1;
        }
        if (n == 0)
            break;
        total += n;
    }
    return total;
}

/* ══════════════════════════════════════════════════════════════════════
 *  F6. A page landing exactly on the 75-char limit must not lose data
 *  Exposes: off-by-one page-boundary arithmetic dropping a value
 * ══════════════════════════════════════════════════════════════════════ */
static test_result_t test_fault_page_boundary(timing_ctx_t *ctx, char addr)
{
    test_result_t r = TEST_RESULT("Exact 75-Char Page Boundary",
                                  SECT_SELFTEST);
    hal_loopback_set_faults(ctx->hal, LB_FAULT_BOUNDARY_VALUES);

    sdi12_meas_response_t mresp;
    int total = ha_fetch_all(ctx, addr, &mresp, &r);

    hal_loopback_set_faults(ctx->hal, LB_FAULT_NONE);
    if (total < 0) return r;

    if (total != mresp.value_count) {
        TEST_FAIL_MSG(r, "HA promised %d values, D pages delivered %d — "
                      "the value landing on the exact 75-char boundary "
                      "was silently lost", mresp.value_count, total);
        return r;
    }

    TEST_PASS_MSG(r, "All %d values delivered across boundary-exact pages",
                  total);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  F7. Overwide value must be precision-reduced, not truncated
 *  Exposes: formatter emitting grammar-invalid output like "+9999999."
 * ══════════════════════════════════════════════════════════════════════ */
static test_result_t test_fault_overwide_value(timing_ctx_t *ctx, char addr)
{
    test_result_t r = TEST_RESULT("Overwide Value Precision Reduction",
                                  SECT_SELFTEST);
    hal_loopback_set_faults(ctx->hal, LB_FAULT_OVERWIDE_VALUE);

    sdi12_meas_response_t mresp;
    int total = ha_fetch_all(ctx, addr, &mresp, &r);

    hal_loopback_set_faults(ctx->hal, LB_FAULT_NONE);
    if (total < 0) return r;

    if (total != mresp.value_count) {
        TEST_FAIL_MSG(r, "HA promised %d values, D pages delivered %d",
                      mresp.value_count, total);
        return r;
    }

    TEST_PASS_MSG(r, "7-digit + 6-decimal value formatted grammar-clean");
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  F8. CRC engine matches the spec's own worked examples (§4.4.12.3)
 *  Breaks the symmetric-implementation blindness for the CRC itself
 * ══════════════════════════════════════════════════════════════════════ */
static test_result_t test_fault_crc_vectors(timing_ctx_t *ctx, char addr)
{
    (void)ctx; (void)addr;
    test_result_t r = TEST_RESULT("Spec CRC Worked Examples", SECT_SELFTEST);

    char a[32] = "0+3.14";
    char b[48] = "0+1.11+2.22+3.33+4.44+5.55+6.66";
    if (sdi12_crc_append(a, sizeof(a)) != SDI12_OK ||
        strcmp(a, "0+3.14OqZ\r\n") != 0) {
        TEST_FAIL_MSG(r, "\"0+3.14\" CRC != OqZ (got \"%.12s\")", a);
        return r;
    }
    if (sdi12_crc_append(b, sizeof(b)) != SDI12_OK ||
        strcmp(b, "0+1.11+2.22+3.33+4.44+5.55+6.66I]q\r\n") != 0) {
        TEST_FAIL_MSG(r, "six-value example CRC != I]q");
        return r;
    }

    TEST_PASS_MSG(r, "OqZ and I]q reproduce byte-for-byte");
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  F9. Master HV API must accept the spec's empty terminator page
 *  Exposes: get_hv_data trimming CR/LF before the raw-length CRC guard
 * ══════════════════════════════════════════════════════════════════════ */
static test_result_t test_fault_hv_terminator(timing_ctx_t *ctx, char addr)
{
    test_result_t r = TEST_RESULT("HV Terminator Page via Master API",
                                  SECT_SELFTEST);

    sdi12_master_send_break(timing_master(ctx));

    sdi12_meas_response_t mresp;
    sdi12_err_t err = sdi12_master_start_measurement(
        timing_master(ctx), addr, SDI12_MEAS_HIGHVOL_ASCII, 0, false, &mresp);
    if (err != SDI12_OK) {
        TEST_FAIL_MSG(r, "HA command failed with error %d", err);
        return r;
    }

    /* Page through sdi12_master_get_hv_data (the documented paginate-
     * until-empty pattern) — the terminating page is a<CRC><CR><LF>
     * (§5.1 empty page, e.g. 0AP@) and must come back as OK with
     * raw_len == 0, not as a CRC error. */
    uint16_t total = 0;
    for (uint16_t page = 0; page < 1000; page++) {
        char raw[96];
        size_t raw_len = sizeof(raw);
        err = sdi12_master_get_hv_data(timing_master(ctx), addr, page,
                                       raw, &raw_len);
        if (err != SDI12_OK) {
            TEST_FAIL_MSG(r, "D%u! failed with error %d — the compliant "
                          "empty terminator page must not be an error",
                          page, err);
            return r;
        }
        if (raw_len == 0)
            break;  /* clean end-of-data */

        sdi12_value_t vals[SDI12_MAX_VALUES];
        uint8_t n = 0;
        sdi12_master_parse_data_values(raw, raw_len, vals,
                                       SDI12_MAX_VALUES, &n, false);
        if (n == 0) break;
        total += n;
    }

    if (total != mresp.value_count) {
        TEST_FAIL_MSG(r, "HA promised %d values, pages delivered %d",
                      mresp.value_count, total);
        return r;
    }

    TEST_PASS_MSG(r, "%d values plus a clean empty-terminator page", total);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  F10. Binary payload must never be silently truncated under a valid CRC
 *  Exposes: dishonest buflen handed to format_binary_page + clamp
 * ══════════════════════════════════════════════════════════════════════ */
static test_result_t test_fault_fat_binary(timing_ctx_t *ctx, char addr)
{
    test_result_t r = TEST_RESULT("Binary Payload Fills Documented Buffer",
                                  SECT_SELFTEST);
    hal_loopback_set_faults(ctx->hal, LB_FAULT_FAT_BINARY);

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
        TEST_FAIL_MSG(r, "DB0! failed with error %d", err);
        return r;
    }
    if (out_len < 2) {
        TEST_FAIL_MSG(r, "DB0! delivered %zu bytes — too short to check",
                      out_len);
        return r;
    }

    /* The callback embedded the payload length it actually wrote in
     * payload[0..1]. If the library truncated after the fact, the
     * packet still CRC-verifies — only this check can see the loss. */
    size_t written = (size_t)payload[0] | ((size_t)payload[1] << 8);
    if (written != out_len) {
        TEST_FAIL_MSG(r, "Callback wrote %zu payload bytes per its "
                      "documented buflen, wire delivered %zu — silent "
                      "truncation under a valid CRC", written, out_len);
        return r;
    }

    TEST_PASS_MSG(r, "%zu payload bytes written == %zu delivered",
                  written, out_len);
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
    test_suite_add(suite, "fault_paced",
                   "Paced-UART Response Delivery", SECT_SELFTEST,
                   test_fault_paced_uart);
    test_suite_add(suite, "fault_boundary",
                   "Exact 75-Char Page Boundary", SECT_SELFTEST,
                   test_fault_page_boundary);
    test_suite_add(suite, "fault_overwide",
                   "Overwide Value Precision Reduction", SECT_SELFTEST,
                   test_fault_overwide_value);
    test_suite_add(suite, "fault_crc_vectors",
                   "Spec CRC Worked Examples", SECT_SELFTEST,
                   test_fault_crc_vectors);
    test_suite_add(suite, "fault_hv_term",
                   "HV Terminator Page via Master API", SECT_SELFTEST,
                   test_fault_hv_terminator);
    test_suite_add(suite, "fault_fat_bin",
                   "Binary Payload Fills Documented Buffer", SECT_SELFTEST,
                   test_fault_fat_binary);
}
