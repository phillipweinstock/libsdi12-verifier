/**
 * @file timing.h
 * @brief Timing interposition layer — wraps libsdi12 master callbacks
 *        with microsecond timestamp capture.
 *
 * This is the core innovation of the verifier: it interposes on every
 * send/recv/break callback to record exact timestamps, enabling precise
 * measurement of response latency, inter-character gaps, break duration,
 * and service request timing — all without modifying libsdi12.
 */
#ifndef TIMING_H
#define TIMING_H

#include "hal.h"
#include "sdi12.h"
#include "sdi12_master.h"

/** Maximum bytes to timestamp per transaction. */
#define TIMING_MAX_BYTES 256

/**
 * @brief Timing-instrumented master context.
 *
 * Wraps sdi12_master_ctx_t with timestamp capture on every bus event.
 */
typedef struct {
    /* libsdi12 master context */
    sdi12_master_ctx_t  master;

    /* HAL reference */
    hal_t              *hal;

    /* Timestamp captures (microseconds since boot) */
    uint64_t            break_start_us;
    uint64_t            break_end_us;
    uint64_t            cmd_start_us;
    uint64_t            cmd_end_us;
    uint64_t            resp_start_us;
    uint64_t            resp_end_us;

    /* Per-byte timestamps for inter-character gap analysis */
    uint64_t            byte_times_us[TIMING_MAX_BYTES];
    size_t              byte_count;

    /* Configuration */
    bool                use_rts;
} timing_ctx_t;

/**
 * Initialize timing context — sets up libsdi12 master with interposed
 * callbacks that capture microsecond timestamps on every bus event.
 *
 * @param ctx      Timing context (caller-allocated).
 * @param hal      Platform HAL (must already be open).
 * @param use_rts  Use RTS line for TX/RX direction control.
 * @return 0 on success, -1 on failure.
 */
int timing_init(timing_ctx_t *ctx, hal_t *hal, bool use_rts);

/** Microseconds between command TX completion and first response byte. */
uint64_t timing_response_latency_us(const timing_ctx_t *ctx);

/** Microseconds of the break + marking sequence. */
uint64_t timing_break_duration_us(const timing_ctx_t *ctx);

/** Largest inter-character gap in the most recent response (microseconds). */
uint64_t timing_max_interchar_gap_us(const timing_ctx_t *ctx);

/** Get the underlying libsdi12 master context for API calls. */
static inline sdi12_master_ctx_t *timing_master(timing_ctx_t *ctx) {
    return &ctx->master;
}

#endif /* TIMING_H */
