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

    /** format_binary_page follows the contract as documented in
     *  sdi12_sensor.h verbatim (payload bytes written starting at
     *  buf[1], return count excludes any type prefix). If library code
     *  and documentation disagree, packets mis-frame on the wire. */
    LB_FAULT_DOC_CONTRACT_BIN = 1u << 2,
} lb_fault_t;

/** Set the active fault mask on a loopback HAL (LB_FAULT_NONE clears). */
void hal_loopback_set_faults(hal_t *hal, unsigned faults);

#endif /* HAL_LOOPBACK_H */
