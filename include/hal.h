/**
 * @file hal.h
 * @brief Hardware Abstraction Layer — serial port + microsecond timing.
 *
 * Provides a platform-agnostic interface for serial I/O, bus control,
 * and high-resolution timing.  Implementations live in hal/hal_win32.c
 * and hal/hal_posix.c.
 */
#ifndef HAL_H
#define HAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct hal hal_t;

/** Serial port configuration. */
typedef struct {
    const char *port;       /**< e.g. "COM3" or "/dev/ttyUSB0"       */
    uint32_t    baud;       /**< Baud rate (default 1200)             */
    uint8_t     data_bits;  /**< 7 for SDI-12                        */
    uint8_t     parity;     /**< 0=none, 1=odd, 2=even               */
    uint8_t     stop_bits;  /**< 1 or 2                              */
} hal_serial_config_t;

/**
 * @brief HAL interface — all platform interaction goes through here.
 */
struct hal {
    /* Serial I/O */
    int      (*open)(hal_t *self, const hal_serial_config_t *cfg);
    void     (*close)(hal_t *self);
    size_t   (*write)(hal_t *self, const char *data, size_t len);
    size_t   (*read)(hal_t *self, char *buf, size_t max, uint32_t timeout_ms);
    void     (*flush)(hal_t *self);

    /* Bus control */
    void     (*set_break)(hal_t *self, bool enable);
    void     (*set_rts)(hal_t *self, bool high);

    /* High-resolution timing */
    uint64_t (*micros)(hal_t *self);
    void     (*delay_ms)(hal_t *self, uint32_t ms);
    void     (*delay_us)(hal_t *self, uint64_t us);

    /* Private data (platform-specific) */
    void *priv;
};

/** Default SDI-12 serial configuration: 1200 baud, 7 data bits, even parity, 1 stop. */
static inline hal_serial_config_t hal_sdi12_default(const char *port) {
    hal_serial_config_t cfg;
    cfg.port      = port;
    cfg.baud      = 1200;
    cfg.data_bits = 7;
    cfg.parity    = 2;   /* even */
    cfg.stop_bits = 1;
    return cfg;
}

/** Create a HAL instance for the current platform. */
hal_t *hal_create_default(void);

/** Create an in-memory loopback HAL with a simulated sensor (no hardware). */
hal_t *hal_create_loopback(char addr);

/** Destroy a HAL instance and free resources. */
void   hal_destroy(hal_t *hal);

#endif /* HAL_H */
