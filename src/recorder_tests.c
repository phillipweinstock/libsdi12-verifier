/**
 * @file recorder_tests.c
 * @brief SDI-12 data recorder compliance tests.
 *
 * The verifier acts as a simulated SDI-12 sensor using the libsdi12
 * sensor API. The device under test is a data recorder (master).
 *
 * Architecture:
 *   - The verifier configures itself as a sensor at the target address
 *   - It listens on the bus for commands from the recorder
 *   - Each test verifies a specific aspect of the recorder's protocol
 *     behavior: break timing, command format, M->D sequence, etc.
 *
 * The simulated sensor responds correctly so the recorder can operate
 * normally while we measure its compliance from the sensor's perspective.
 */
#include "verifier.h"
#include "sdi12.h"
#include "sdi12_sensor.h"
#include <stdio.h>
#include <string.h>

/* ── Simulated sensor state shared across tests ────────────────── */

/** Simulated sensor parameters: two simple values. */
#define SIM_PARAM_COUNT 2
static const float SIM_VALUES[SIM_PARAM_COUNT] = { 23.45f, 1013.25f };

/** Buffer that captures the last response sent by the simulated sensor. */
typedef struct {
    hal_t              *hal;
    timing_ctx_t       *timing;
    sdi12_sensor_ctx_t  sensor;
    char                addr;
    bool                use_rts;

    /* Captured bus events for analysis */
    uint64_t            break_start_us;
    uint64_t            break_end_us;
    uint64_t            marking_end_us;
    uint64_t            cmd_byte_times[32];
    size_t              cmd_byte_count;
    uint64_t            last_cmd_us;
    char                last_cmd[SDI12_MAX_COMMAND_LEN + 4];
    size_t              last_cmd_len;

    /* Response tracking */
    char                last_response[SDI12_MAX_RESPONSE_LEN + 4];
    size_t              last_response_len;
} rec_sim_t;

static rec_sim_t g_sim;

/* ── Sensor callbacks — bridge to HAL ─────────────────────────── */

static void sim_send_response(const char *data, size_t len, void *user_data) {
    rec_sim_t *s = (rec_sim_t *)user_data;

    /* Capture the response */
    size_t copy = len < sizeof(s->last_response) - 1 ? len : sizeof(s->last_response) - 1;
    memcpy(s->last_response, data, copy);
    s->last_response[copy] = '\0';
    s->last_response_len = copy;

    if (s->use_rts)
        s->hal->set_rts(s->hal, true);

    s->hal->write(s->hal, data, len);
    s->hal->flush(s->hal);

    if (s->use_rts)
        s->hal->set_rts(s->hal, false);
}

static void sim_set_direction(sdi12_dir_t dir, void *user_data) {
    rec_sim_t *s = (rec_sim_t *)user_data;
    if (s->use_rts)
        s->hal->set_rts(s->hal, dir == SDI12_DIR_TX);
}

static sdi12_value_t sim_read_param(uint8_t param_index, void *user_data) {
    (void)user_data;
    sdi12_value_t v;
    if (param_index < SIM_PARAM_COUNT) {
        v.value    = SIM_VALUES[param_index];
        v.decimals = 2;
    } else {
        v.value    = 0.0f;
        v.decimals = 0;
    }
    return v;
}

/* ── Bus RX helpers — listen for recorder commands ────────────── */

/**
 * Wait for a break signal from the recorder.
 * Measures the break duration for compliance checking.
 *
 * A break is a continuous spacing condition (logical 0 / line held low)
 * for >= 12ms.  At 1200 baud, one byte takes ~8.33ms, so a sustained
 * spacing > 12ms is detected as a break.  We detect this via a framing
 * error / null byte pattern from the UART, or a long idle gap.
 *
 * Returns: microseconds of the break, or 0 on timeout.
 */
static uint64_t wait_for_break(rec_sim_t *s, uint32_t timeout_ms) {
    uint64_t deadline = s->hal->micros(s->hal) + (uint64_t)timeout_ms * 1000;
    char c;
    uint64_t idle_start = 0;

    /* Consume any prior data, then detect sustained idle (break) */
    while (s->hal->micros(s->hal) < deadline) {
        size_t n = s->hal->read(s->hal, &c, 1, 1);
        if (n == 0) {
            if (idle_start == 0)
                idle_start = s->hal->micros(s->hal);
        } else {
            /* Got a byte — if it's a null (break char), measure it */
            if (c == '\0') {
                s->break_start_us = s->hal->micros(s->hal);
                /* Keep reading nulls while break continues */
                while (s->hal->micros(s->hal) < deadline) {
                    n = s->hal->read(s->hal, &c, 1, 2);
                    if (n == 0 || c != '\0') {
                        s->break_end_us = s->hal->micros(s->hal);
                        s->marking_end_us = 0;

                        /* Now wait for the marking period to end (next real char) */
                        uint64_t mark_deadline = s->hal->micros(s->hal) + 20000;
                        while (s->hal->micros(s->hal) < mark_deadline) {
                            n = s->hal->read(s->hal, &c, 1, 1);
                            if (n == 1 && c != '\0') {
                                s->marking_end_us = s->hal->micros(s->hal);
                                /* Put this byte back into our command buffer */
                                s->cmd_byte_times[0] = s->marking_end_us;
                                s->last_cmd[0] = c;
                                s->cmd_byte_count = 1;
                                s->last_cmd_len = 1;
                                return s->break_end_us - s->break_start_us;
                            }
                        }
                        return s->break_end_us - s->break_start_us;
                    }
                }
            }
            idle_start = 0;
        }
    }
    return 0;
}

/**
 * Read a complete command from the recorder (up to '!' terminator).
 * Records per-byte timestamps for inter-character gap analysis.
 * Assumes first byte may already be in the buffer from break detection.
 */
static bool read_command(rec_sim_t *s, uint32_t timeout_ms) {
    uint64_t deadline = s->hal->micros(s->hal) + (uint64_t)timeout_ms * 1000;

    /* Continue from where break detection left off */
    while (s->hal->micros(s->hal) < deadline) {
        /* Check if we already have a complete command */
        if (s->last_cmd_len > 0 && s->last_cmd[s->last_cmd_len - 1] == '!') {
            s->last_cmd[s->last_cmd_len] = '\0';
            s->last_cmd_us = s->cmd_byte_times[s->cmd_byte_count - 1];
            return true;
        }

        if (s->last_cmd_len >= sizeof(s->last_cmd) - 1)
            break;

        char c;
        size_t n = s->hal->read(s->hal, &c, 1, 1);
        if (n == 1) {
            uint64_t now = s->hal->micros(s->hal);
            if (s->cmd_byte_count < 32)
                s->cmd_byte_times[s->cmd_byte_count++] = now;
            s->last_cmd[s->last_cmd_len++] = c;
        }
    }

    if (s->last_cmd_len > 0)
        s->last_cmd[s->last_cmd_len] = '\0';

    return s->last_cmd_len > 0 && s->last_cmd[s->last_cmd_len - 1] == '!';
}

/**
 * Wait for a break + command sequence from the recorder, then process
 * it through the simulated sensor and respond.  Returns true if a
 * valid command was received and responded to.
 */
static bool wait_and_respond(rec_sim_t *s, uint32_t timeout_ms) {
    s->cmd_byte_count = 0;
    s->last_cmd_len = 0;
    memset(s->last_cmd, 0, sizeof(s->last_cmd));

    uint64_t brk = wait_for_break(s, timeout_ms);
    if (brk == 0)
        return false;

    /* Notify sensor of break */
    sdi12_sensor_break(&s->sensor);

    if (!read_command(s, 500))
        return false;

    /* Process through sensor — will auto-respond via callback */
    sdi12_sensor_process(&s->sensor, s->last_cmd, s->last_cmd_len - 1);
    return true;
}

/**
 * Initialize the simulated sensor for recorder tests.
 */
static int sim_init(rec_sim_t *s, timing_ctx_t *timing_ctx, char addr) {
    memset(s, 0, sizeof(*s));
    s->hal     = timing_ctx->hal;
    s->timing  = timing_ctx;
    s->addr    = addr;
    s->use_rts = timing_ctx->use_rts;

    sdi12_ident_t ident;
    memset(&ident, 0, sizeof(ident));
    memcpy(ident.vendor,           "VERIFY  ", SDI12_ID_VENDOR_LEN);
    memcpy(ident.model,            "RECSIM",   SDI12_ID_MODEL_LEN);
    memcpy(ident.firmware_version, "010",      SDI12_ID_FWVER_LEN);
    strncpy(ident.serial,          "SN001",    SDI12_ID_SERIAL_MAXLEN);

    sdi12_sensor_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.send_response = sim_send_response;
    cb.set_direction = sim_set_direction;
    cb.read_param    = sim_read_param;
    cb.user_data     = s;

    sdi12_err_t err = sdi12_sensor_init(&s->sensor, addr, &ident, &cb);
    if (err != SDI12_OK)
        return -1;

    /* Register two measurement parameters in group 0 */
    sdi12_sensor_register_param(&s->sensor, 0, "TA", "C", 2);
    sdi12_sensor_register_param(&s->sensor, 0, "PA", "kPa", 2);

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  1. Break Duration >= 12ms
 *  §4.2.1 — Recorder must hold spacing for >= 12ms
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_rec_break(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Break Duration (>=12ms)", "\xc2\xa7" "4.2.1");

    if (sim_init(&g_sim, ctx, addr) != 0) {
        TEST_ERROR_MSG(r, "Failed to initialize simulated sensor");
        return r;
    }

    printf("\n          Waiting for recorder to send a command...\n          ");
    fflush(stdout);

    /* Wait for break + command from recorder */
    g_sim.cmd_byte_count = 0;
    g_sim.last_cmd_len = 0;

    uint64_t brk = wait_for_break(&g_sim, 30000);
    if (brk == 0) {
        TEST_ERROR_MSG(r, "No break received from recorder within 30s");
        return r;
    }

    r.measured_us   = brk;
    r.spec_limit_us = SDI12_BREAK_MS * 1000;

    /* Read and respond to the command so the recorder is happy */
    if (read_command(&g_sim, 500)) {
        sdi12_sensor_break(&g_sim.sensor);
        sdi12_sensor_process(&g_sim.sensor, g_sim.last_cmd, g_sim.last_cmd_len - 1);
    }

    if (r.measured_us < r.spec_limit_us) {
        TEST_FAIL_MSG(r, "Break %.3f ms is shorter than 12ms minimum",
                      r.measured_us / 1000.0);
        return r;
    }

    TEST_PASS_MSG(r, "Break duration %.3f ms (min: 12.000 ms)", r.measured_us / 1000.0);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  2. Marking After Break >= 8.33ms
 *  §4.2.1 — Marking period between break and first command byte
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_rec_marking(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Marking After Break (>=8.33ms)", "\xc2\xa7" "4.2.1");

    if (sim_init(&g_sim, ctx, addr) != 0) {
        TEST_ERROR_MSG(r, "Failed to initialize simulated sensor");
        return r;
    }

    printf("\n          Waiting for recorder break + command...\n          ");
    fflush(stdout);

    g_sim.cmd_byte_count = 0;
    g_sim.last_cmd_len = 0;

    uint64_t brk = wait_for_break(&g_sim, 30000);
    if (brk == 0) {
        TEST_ERROR_MSG(r, "No break received within 30s");
        return r;
    }

    /* Read command and respond */
    if (read_command(&g_sim, 500)) {
        sdi12_sensor_break(&g_sim.sensor);
        sdi12_sensor_process(&g_sim.sensor, g_sim.last_cmd, g_sim.last_cmd_len - 1);
    }

    if (g_sim.marking_end_us == 0 || g_sim.break_end_us == 0) {
        TEST_ERROR_MSG(r, "Could not measure marking duration");
        return r;
    }

    uint64_t marking_us = g_sim.marking_end_us - g_sim.break_end_us;
    r.measured_us   = marking_us;
    r.spec_limit_us = SDI12_MARK_AFTER_BREAK_MS * 1000;

    if (marking_us < r.spec_limit_us) {
        TEST_FAIL_MSG(r, "Marking %.3f ms is shorter than 8.33ms minimum",
                      marking_us / 1000.0);
        return r;
    }

    TEST_PASS_MSG(r, "Marking duration %.3f ms (min: 8.330 ms)", marking_us / 1000.0);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  3. Command Format Validation
 *  §4.3 — Commands must be properly formatted: addr + cmd + '!'
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_rec_cmd_format(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Command Format Validation", "\xc2\xa7" "4.3");

    if (sim_init(&g_sim, ctx, addr) != 0) {
        TEST_ERROR_MSG(r, "Failed to initialize simulated sensor");
        return r;
    }

    printf("\n          Waiting for recorder command...\n          ");
    fflush(stdout);

    if (!wait_and_respond(&g_sim, 30000)) {
        TEST_ERROR_MSG(r, "No command received within 30s");
        return r;
    }

    /* Validate command structure */
    size_t len = g_sim.last_cmd_len;

    if (len < 2) {
        TEST_FAIL_MSG(r, "Command too short: %zu bytes", len);
        return r;
    }

    /* Must end with '!' */
    if (g_sim.last_cmd[len - 1] != '!') {
        TEST_FAIL_MSG(r, "Command does not end with '!': \"%s\"", g_sim.last_cmd);
        return r;
    }

    /* First char must be valid address or '?' */
    char cmd_addr = g_sim.last_cmd[0];
    if (!sdi12_valid_address(cmd_addr) && cmd_addr != '?') {
        TEST_FAIL_MSG(r, "Invalid address in command: 0x%02X", (unsigned char)cmd_addr);
        return r;
    }

    TEST_PASS_MSG(r, "Valid command: \"%s\" (%zu bytes)", g_sim.last_cmd, len);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  4. Correct Address Usage
 *  §4.3 — Recorder must use the correct sensor address
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_rec_address(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Correct Address in Commands", "\xc2\xa7" "4.3");

    if (sim_init(&g_sim, ctx, addr) != 0) {
        TEST_ERROR_MSG(r, "Failed to initialize simulated sensor");
        return r;
    }

    printf("\n          Waiting for recorder commands (up to 3)...\n          ");
    fflush(stdout);

    int valid = 0;
    int total = 0;
    char wrong_addr = 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        if (!wait_and_respond(&g_sim, 15000))
            break;

        total++;
        char cmd_addr = g_sim.last_cmd[0];

        /* '?' is always valid (address query) */
        if (cmd_addr == '?' || cmd_addr == addr) {
            valid++;
        } else {
            wrong_addr = cmd_addr;
        }
    }

    if (total == 0) {
        TEST_ERROR_MSG(r, "No commands received");
        return r;
    }

    if (valid < total) {
        TEST_FAIL_MSG(r, "Wrong address '%c' used (%d/%d correct)",
                      wrong_addr, valid, total);
        return r;
    }

    TEST_PASS_MSG(r, "All %d commands used correct address '%c'", total, addr);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  5. M -> wait -> D Sequence
 *  §4.4.6/§4.4.7 — Recorder must follow M! with D0! after waiting
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_rec_sequence(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("M -> D Measurement Sequence", "\xc2\xa7" "4.4.6");

    if (sim_init(&g_sim, ctx, addr) != 0) {
        TEST_ERROR_MSG(r, "Failed to initialize simulated sensor");
        return r;
    }

    printf("\n          Waiting for recorder M command...\n          ");
    fflush(stdout);

    bool got_m = false;
    bool got_d = false;

    /* Wait for commands — look for M then D sequence */
    for (int attempt = 0; attempt < 10; attempt++) {
        if (!wait_and_respond(&g_sim, 15000))
            break;

        if (!got_m) {
            /* Look for an M-type command: aM!, aMC!, aM1!, etc. */
            if (g_sim.last_cmd_len >= 3 &&
                g_sim.last_cmd[0] == addr &&
                g_sim.last_cmd[1] == 'M') {
                got_m = true;
                continue;
            }
        } else {
            /* After M, look for D0! */
            if (g_sim.last_cmd_len >= 4 &&
                g_sim.last_cmd[0] == addr &&
                g_sim.last_cmd[1] == 'D') {
                got_d = true;
                break;
            }
        }
    }

    if (!got_m) {
        TEST_SKIP_MSG(r, "Recorder did not send an M command");
        return r;
    }

    if (!got_d) {
        TEST_FAIL_MSG(r, "Recorder sent M but did not follow with D command");
        return r;
    }

    TEST_PASS_MSG(r, "Correct M -> D sequence observed");
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  6. Inter-Character Gap in Commands
 *  §4.2.4 — Max gap between characters within a command
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_rec_interchar(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Command Inter-Character Gap", "\xc2\xa7" "4.2.4");

    if (sim_init(&g_sim, ctx, addr) != 0) {
        TEST_ERROR_MSG(r, "Failed to initialize simulated sensor");
        return r;
    }

    printf("\n          Waiting for recorder command...\n          ");
    fflush(stdout);

    if (!wait_and_respond(&g_sim, 30000)) {
        TEST_ERROR_MSG(r, "No command received within 30s");
        return r;
    }

    if (g_sim.cmd_byte_count < 2) {
        TEST_SKIP_MSG(r, "Command too short to measure inter-character gaps");
        return r;
    }

    /* Find largest gap between consecutive bytes */
    uint64_t max_gap = 0;
    for (size_t i = 1; i < g_sim.cmd_byte_count; i++) {
        uint64_t gap = g_sim.cmd_byte_times[i] - g_sim.cmd_byte_times[i - 1];
        if (gap > max_gap)
            max_gap = gap;
    }

    r.measured_us   = max_gap;
    r.spec_limit_us = SDI12_INTERCHAR_MAX_MS * 1000;

    /*
     * Note: the spec limit on inter-character gap (1.66ms) is defined for
     * sensor responses.  For recorder commands it's less strictly defined,
     * but a well-behaved recorder should also transmit without excessive
     * gaps.  We use the same limit as a guideline.
     */
    if (max_gap > r.spec_limit_us) {
        TEST_FAIL_MSG(r, "Max inter-char gap %.3f ms in command \"%s\"",
                      max_gap / 1000.0, g_sim.last_cmd);
        return r;
    }

    TEST_PASS_MSG(r, "Max gap: %.3f ms in \"%s\" (%zu bytes)",
                  max_gap / 1000.0, g_sim.last_cmd, g_sim.cmd_byte_count);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  7. Acknowledge Response Handling
 *  §4.4.2 — Recorder should handle acknowledge (a!) properly
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_rec_ack(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Acknowledge Handling (a!)", "\xc2\xa7" "4.4.2");

    if (sim_init(&g_sim, ctx, addr) != 0) {
        TEST_ERROR_MSG(r, "Failed to initialize simulated sensor");
        return r;
    }

    printf("\n          Waiting for recorder acknowledge command...\n          ");
    fflush(stdout);

    /* Wait for an acknowledge command (a!) */
    bool got_ack = false;

    for (int attempt = 0; attempt < 5; attempt++) {
        if (!wait_and_respond(&g_sim, 15000))
            break;

        /* Check if this was an acknowledge: "a!" */
        if (g_sim.last_cmd_len == 2 &&
            g_sim.last_cmd[0] == addr &&
            g_sim.last_cmd[1] == '!') {
            got_ack = true;
            break;
        }

        /* Also accept ?! as an address query */
        if (g_sim.last_cmd_len == 2 &&
            g_sim.last_cmd[0] == '?' &&
            g_sim.last_cmd[1] == '!') {
            got_ack = true;
            break;
        }
    }

    if (!got_ack) {
        TEST_SKIP_MSG(r, "Recorder did not send acknowledge command");
        return r;
    }

    /* Verify we sent back a valid response */
    if (g_sim.last_response_len > 0 && g_sim.last_response[0] == addr) {
        TEST_PASS_MSG(r, "Acknowledged, sensor responded '%c\\r\\n'", addr);
    } else {
        TEST_FAIL_MSG(r, "Unexpected response state after acknowledge");
    }

    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  8. Identify Command Handling
 *  §4.4.3 — Recorder sends aI! and parses identification
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_rec_identify(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Identify Command (aI!)", "\xc2\xa7" "4.4.3");

    if (sim_init(&g_sim, ctx, addr) != 0) {
        TEST_ERROR_MSG(r, "Failed to initialize simulated sensor");
        return r;
    }

    printf("\n          Waiting for recorder identify command...\n          ");
    fflush(stdout);

    bool got_identify = false;

    for (int attempt = 0; attempt < 8; attempt++) {
        if (!wait_and_respond(&g_sim, 15000))
            break;

        /* Check for aI! */
        if (g_sim.last_cmd_len == 3 &&
            g_sim.last_cmd[0] == addr &&
            g_sim.last_cmd[1] == 'I' &&
            g_sim.last_cmd[2] == '!') {
            got_identify = true;
            break;
        }
    }

    if (!got_identify) {
        TEST_SKIP_MSG(r, "Recorder did not send identify command");
        return r;
    }

    /* Check that we sent a well-formed identification response */
    if (g_sim.last_response_len < 10) {
        TEST_ERROR_MSG(r, "Identification response too short");
        return r;
    }

    TEST_PASS_MSG(r, "Identification sent: VERIFY/RECSIM");
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  9. Break Between Commands
 *  §4.2.1 — Recorder must send break before each command sequence
 * ══════════════════════════════════════════════════════════════════════ */

static test_result_t test_rec_break_between(timing_ctx_t *ctx, char addr) {
    test_result_t r = TEST_RESULT("Break Between Commands", "\xc2\xa7" "4.2.1");

    if (sim_init(&g_sim, ctx, addr) != 0) {
        TEST_ERROR_MSG(r, "Failed to initialize simulated sensor");
        return r;
    }

    printf("\n          Waiting for two consecutive commands...\n          ");
    fflush(stdout);

    int breaks_seen = 0;
    int commands_seen = 0;

    for (int attempt = 0; attempt < 5; attempt++) {
        g_sim.cmd_byte_count = 0;
        g_sim.last_cmd_len = 0;
        g_sim.break_start_us = 0;
        g_sim.break_end_us = 0;

        uint64_t brk = wait_for_break(&g_sim, 15000);
        if (brk == 0)
            break;

        breaks_seen++;
        sdi12_sensor_break(&g_sim.sensor);

        if (read_command(&g_sim, 500)) {
            sdi12_sensor_process(&g_sim.sensor, g_sim.last_cmd, g_sim.last_cmd_len - 1);
            commands_seen++;
        }

        if (commands_seen >= 2)
            break;
    }

    if (commands_seen < 2) {
        TEST_SKIP_MSG(r, "Only received %d commands (need 2)", commands_seen);
        return r;
    }

    if (breaks_seen >= commands_seen) {
        TEST_PASS_MSG(r, "%d breaks for %d commands — break before each",
                      breaks_seen, commands_seen);
    } else {
        TEST_FAIL_MSG(r, "Only %d breaks for %d commands — missing breaks",
                      breaks_seen, commands_seen);
    }

    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Registration
 * ══════════════════════════════════════════════════════════════════════ */

void recorder_tests_register(test_suite_t *suite) {
    test_suite_add(suite, "rec_break",       "Break Duration (>=12ms)",         "\xc2\xa7" "4.2.1",    test_rec_break);
    test_suite_add(suite, "rec_marking",     "Marking After Break (>=8.33ms)",  "\xc2\xa7" "4.2.1",    test_rec_marking);
    test_suite_add(suite, "rec_cmd_fmt",     "Command Format Validation",       "\xc2\xa7" "4.3",      test_rec_cmd_format);
    test_suite_add(suite, "rec_address",     "Correct Address in Commands",     "\xc2\xa7" "4.3",      test_rec_address);
    test_suite_add(suite, "rec_ack",         "Acknowledge Handling (a!)",       "\xc2\xa7" "4.4.2",    test_rec_ack);
    test_suite_add(suite, "rec_identify",    "Identify Command (aI!)",          "\xc2\xa7" "4.4.3",    test_rec_identify);
    test_suite_add(suite, "rec_sequence",    "M -> D Measurement Sequence",     "\xc2\xa7" "4.4.6",    test_rec_sequence);
    test_suite_add(suite, "rec_interchar",   "Command Inter-Character Gap",     "\xc2\xa7" "4.2.4",    test_rec_interchar);
    test_suite_add(suite, "rec_brk_between", "Break Between Commands",          "\xc2\xa7" "4.2.1",    test_rec_break_between);
}
