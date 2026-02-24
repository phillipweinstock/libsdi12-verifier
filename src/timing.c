/**
 * @file timing.c
 * @brief Timing interposition layer implementation.
 *
 * Wraps libsdi12's master I/O callbacks with microsecond timestamp
 * capture.  The timing_ctx_t pointer is passed via user_data.
 */
#include "timing.h"
#include <string.h>

/* ══════════════════════════════════════════════════════════════════════
 *  Interposed Callbacks
 * ══════════════════════════════════════════════════════════════════════ */

static void cb_send(const char *data, size_t len, void *user_data) {
    timing_ctx_t *t = (timing_ctx_t *)user_data;

    t->cmd_start_us = t->hal->micros(t->hal);

    if (t->use_rts)
        t->hal->set_rts(t->hal, true);

    t->hal->write(t->hal, data, len);
    t->hal->flush(t->hal);

    t->cmd_end_us = t->hal->micros(t->hal);

    if (t->use_rts)
        t->hal->set_rts(t->hal, false);
}

static size_t cb_recv(char *buf, size_t max, uint32_t timeout_ms, void *user_data) {
    timing_ctx_t *t = (timing_ctx_t *)user_data;

    t->byte_count    = 0;
    t->resp_start_us = 0;
    t->resp_end_us   = 0;
    t->line_count    = 0;

    uint64_t deadline = t->hal->micros(t->hal) + (uint64_t)timeout_ms * 1000;
    size_t   pos      = 0;

    while (t->hal->micros(t->hal) < deadline && pos < max) {
        char   c;
        size_t n = t->hal->read(t->hal, &c, 1, 1);   /* 1 ms poll */
        if (n == 1) {
            uint64_t now = t->hal->micros(t->hal);

            if (pos == 0)
                t->resp_start_us = now;

            if (t->byte_count < TIMING_MAX_BYTES)
                t->byte_times_us[t->byte_count++] = now;

            buf[pos++]     = c;
            t->resp_end_us = now;

            if (c == '\n') {
                /* Record line boundary timestamp */
                if (t->line_count < TIMING_MAX_LINES)
                    t->line_end_us[t->line_count++] = now;
                break;
            }
        }
    }

    return pos;
}

static void cb_set_dir(sdi12_dir_t dir, void *user_data) {
    timing_ctx_t *t = (timing_ctx_t *)user_data;
    if (t->use_rts)
        t->hal->set_rts(t->hal, dir == SDI12_DIR_TX);
}

static void cb_break(void *user_data) {
    timing_ctx_t *t = (timing_ctx_t *)user_data;

    t->break_start_us = t->hal->micros(t->hal);

    /* Break: hold line in spacing state for >= 12 ms */
    t->hal->set_break(t->hal, true);
    t->hal->delay_ms(t->hal, 15);
    t->hal->set_break(t->hal, false);

    /* Marking after break: >= 8.33 ms */
    t->hal->delay_ms(t->hal, 9);

    t->break_end_us = t->hal->micros(t->hal);
}

static void cb_delay(uint32_t ms, void *user_data) {
    timing_ctx_t *t = (timing_ctx_t *)user_data;
    t->hal->delay_ms(t->hal, ms);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════════ */

int timing_init(timing_ctx_t *ctx, hal_t *hal, bool use_rts) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->hal     = hal;
    ctx->use_rts = use_rts;

    sdi12_master_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.send          = cb_send;
    cb.recv          = cb_recv;
    cb.set_direction = cb_set_dir;
    cb.send_break    = cb_break;
    cb.delay         = cb_delay;
    cb.user_data     = ctx;

    return sdi12_master_init(&ctx->master, &cb) == SDI12_OK ? 0 : -1;
}

uint64_t timing_response_latency_us(const timing_ctx_t *ctx) {
    if (ctx->resp_start_us == 0 || ctx->cmd_end_us == 0)
        return 0;
    return ctx->resp_start_us - ctx->cmd_end_us;
}

uint64_t timing_break_duration_us(const timing_ctx_t *ctx) {
    if (ctx->break_end_us == 0 || ctx->break_start_us == 0)
        return 0;
    return ctx->break_end_us - ctx->break_start_us;
}

uint64_t timing_max_interchar_gap_us(const timing_ctx_t *ctx) {
    uint64_t max_gap = 0;
    for (size_t i = 1; i < ctx->byte_count; i++) {
        uint64_t gap = ctx->byte_times_us[i] - ctx->byte_times_us[i - 1];
        if (gap > max_gap)
            max_gap = gap;
    }
    return max_gap;
}

uint64_t timing_max_interline_gap_us(const timing_ctx_t *ctx) {
    uint64_t max_gap = 0;
    for (uint8_t i = 1; i < ctx->line_count; i++) {
        uint64_t gap = ctx->line_end_us[i] - ctx->line_end_us[i - 1];
        if (gap > max_gap)
            max_gap = gap;
    }
    return max_gap;
}
