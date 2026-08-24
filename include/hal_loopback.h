/**
 * @file hal_loopback.h
 * @brief Loopback-HAL fault injection for self-test hardening.
 *
 * The loopback HAL embeds a libsdi12-based simulated sensor. By default it
 * behaves as a well-formed, cooperative device — which means a bug present
 * on both sides of the library cancels out on the wire and the self-test
 * cannot see it. These fault modes deliberately break that symmetry:
 * hostile measurement values, a foreign sensor answering binary pages, and
 * a binary formatter written strictly to the libsdi12 documented callback
 * contract rather than to its implementation.
 *
 * Only meaningful for HALs created with hal_create_loopback(); calling
 * hal_loopback_set_faults() on any other HAL is undefined.
 */
#ifndef HAL_LOOPBACK_H
#define HAL_LOOPBACK_H

#include "hal.h"

/** Fault-injection modes (bitmask). */
typedef enum {
    LB_FAULT_NONE             = 0,

    /** read_param returns hostile values: NaN, +inf, 1e30, -0.0f.
     *  A disconnected ADC or failed conversion produces exactly these. */
    LB_FAULT_HOSTILE_VALUES   = 1u << 0,

    /** aDBn! binary packets are re-addressed to a different sensor
     *  address with a correctly recomputed CRC — simulating a foreign
     *  sensor answering on the bus. The master must reject the packet. */
    LB_FAULT_FOREIGN_BIN_ADDR = 1u << 1,

    /** Sensor responses arrive paced at ~2 ms per byte instead of
     *  instantaneously — a mild stand-in for a real 1200-baud line
     *  (8.33 ms per character). Any recv implementation that treats
     *  its timeout as a total-read deadline truncates every response;
     *  the correct contract is timeout-to-first-byte, then read to LF
     *  with an inter-character timeout. */
    LB_FAULT_PACED_UART       = 1u << 3,

    /** read_param returns values whose formatted widths sum to exactly
     *  75 characters — the precise <values> limit of the 75-char page
     *  classes (§4.4.8). Exposes off-by-one page-boundary arithmetic
     *  that silently drops the value landing on the boundary. */
    LB_FAULT_BOUNDARY_VALUES  = 1u << 4,

    /** read_param returns a value whose integer digits + requested
     *  decimals cannot fit the 9-char value cap (e.g. 9999999.0 with
     *  decimals=6). A formatter that truncates instead of reducing
     *  precision emits grammar-invalid output such as "+9999999.". */
    LB_FAULT_OVERWIDE_VALUE   = 1u << 5,
} lb_fault_t;

/** Set the active fault mask on a loopback HAL (LB_FAULT_NONE clears). */
void hal_loopback_set_faults(hal_t *hal, unsigned faults);

#endif /* HAL_LOOPBACK_H */
