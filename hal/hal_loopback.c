/**
 * @file hal_loopback.c
 * @brief Loopback HAL — in-memory sensor for self-testing without hardware.
 *
 * The loopback HAL embeds a simulated SDI-12 sensor (via libsdi12's sensor
 * API) and connects it to the master side through an in-memory ring buffer.
 * No serial port, no threading — everything runs cooperatively:
 *
 *   master write() → command buffer → flush() → sdi12_sensor_process()
 *                                                       ↓
 *   master read()  ← ring buffer   ← send_response callback
 *
 * Timing tests trivially pass (everything is near-instant), but command
 * parsing, CRC, response formatting, and data flow are fully exercised.
 * This provides internal consistency validation — if the verifier is wrong,
 * both sides will be wrong the same way, so it won't catch spec bugs, but
 * it catches regressions, build breakage, and protocol engine mismatches.
 *
 * Usage:
 *   hal_t *hal = hal_create_loopback('0');
 *   // use as drop-in replacement for hal_create_default()
 */
#include "hal.h"
#include "sdi12.h"
#include "sdi12_sensor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

/* ── Ring Buffer ──────────────────────────────────────────────── */

#define LB_RING_SIZE 4096

typedef struct {
    char   buf[LB_RING_SIZE];
    size_t head;    /* write position */
    size_t tail;    /* read position  */
} lb_ring_t;

static size_t lb_ring_avail(const lb_ring_t *r) {
    return (r->head - r->tail + LB_RING_SIZE) % LB_RING_SIZE;
}

static void lb_ring_put(lb_ring_t *r, const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        r->buf[r->head] = data[i];
        r->head = (r->head + 1) % LB_RING_SIZE;
    }
}

static size_t lb_ring_get(lb_ring_t *r, char *out, size_t max) {
    size_t avail = lb_ring_avail(r);
    size_t n = avail < max ? avail : max;
    for (size_t i = 0; i < n; i++) {
        out[i] = r->buf[r->tail];
        r->tail = (r->tail + 1) % LB_RING_SIZE;
    }
    return n;
}

/* ── Private Data ─────────────────────────────────────────────── */

#define LB_CMD_MAX 256

/** Simulated sensor values: temperature + pressure.
 *  Chosen so that their IEEE-754 float32 representations contain no
 *  0x00 (NUL), 0x0A (LF), or 0x0D (CR) bytes — avoids issues with
 *  strlen-based send and newline-based recv for binary transport.    */
#define LB_PARAM_COUNT 2
static const float LB_VALUES[LB_PARAM_COUNT] = { 12.34f, 56.78f };

typedef struct {
    /* Simulated sensor */
    sdi12_sensor_ctx_t sensor;
    char               addr;

    /* Sensor → master response buffer */
    lb_ring_t          resp;

    /* Master → sensor command accumulator */
    char               cmd_buf[LB_CMD_MAX];
    size_t             cmd_len;

    /* Break state tracking */
    bool               break_active;
} loopback_priv_t;

/* ── Sensor Callbacks ─────────────────────────────────────────── */

static void lb_send_response(const char *data, size_t len, void *user_data) {
    loopback_priv_t *p = (loopback_priv_t *)user_data;
    lb_ring_put(&p->resp, data, len);
}

static void lb_set_direction(sdi12_dir_t dir, void *user_data) {
    (void)dir;
    (void)user_data;
}

static sdi12_value_t lb_read_param(uint8_t idx, void *user_data) {
    (void)user_data;
    sdi12_value_t v;
    if (idx < LB_PARAM_COUNT) {
        v.value    = LB_VALUES[idx];
        v.decimals = 2;
    } else {
        v.value    = 0.0f;
        v.decimals = 0;
    }
    return v;
}

/**
 * Format binary data page for HV binary (aHB!) responses.
 * Uses FLOAT32 encoding — each value is a 4-byte IEEE-754 float.
 */
static size_t lb_format_binary_page(uint16_t page,
                                     const sdi12_value_t *values,
                                     uint8_t count,
                                     char *buf, size_t buflen,
                                     void *user_data) {
    (void)user_data;
    if (page > 0) return 0;  /* all values fit on page 0 */

    /* buf[0] = address (already set by library)
     * buf[1] = type prefix byte
     * buf[2..] = raw float data */
    size_t needed = 1 + (size_t)count * sizeof(float);  /* type + data */
    if (needed > buflen - 1) return 0;  /* won't fit */

    buf[1] = (char)SDI12_BINTYPE_FLOAT32;

    for (uint8_t i = 0; i < count; i++) {
        float f = values[i].value;
        memcpy(&buf[2 + i * sizeof(float)], &f, sizeof(float));
    }

    return needed;  /* bytes written starting at buf[1] */
}

/* ── HAL Methods ──────────────────────────────────────────────── */

static int lb_open(hal_t *self, const hal_serial_config_t *cfg) {
    (void)cfg;
    loopback_priv_t *p = (loopback_priv_t *)self->priv;

    sdi12_ident_t ident;
    memset(&ident, 0, sizeof(ident));
    memcpy(ident.vendor,           "LOOPBAK ", SDI12_ID_VENDOR_LEN);
    memcpy(ident.model,            "SELFTS",   SDI12_ID_MODEL_LEN);
    memcpy(ident.firmware_version, "010",      SDI12_ID_FWVER_LEN);
    strncpy(ident.serial,          "LB001",    SDI12_ID_SERIAL_MAXLEN);

    sdi12_sensor_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.send_response = lb_send_response;
    cb.set_direction = lb_set_direction;
    cb.read_param    = lb_read_param;
    cb.format_binary_page = lb_format_binary_page;
    cb.user_data     = p;
    /* No start_measurement/service_request → sync measurements (ttt=0) */

    if (sdi12_sensor_init(&p->sensor, p->addr, &ident, &cb) != SDI12_OK)
        return -1;

    /* Register two parameters in group 0 (M/C/R) */
    sdi12_sensor_register_param(&p->sensor, 0, "TA", "C",   2);
    sdi12_sensor_register_param(&p->sensor, 0, "PA", "kPa", 2);

    return 0;
}

static void lb_close(hal_t *self) {
    (void)self;
}

static size_t lb_write(hal_t *self, const char *data, size_t len) {
    loopback_priv_t *p = (loopback_priv_t *)self->priv;

    for (size_t i = 0; i < len; i++) {
        if (p->cmd_len < LB_CMD_MAX - 1)
            p->cmd_buf[p->cmd_len++] = data[i];
    }

    return len;
}

static void lb_flush(hal_t *self) {
    loopback_priv_t *p = (loopback_priv_t *)self->priv;

    if (p->cmd_len == 0)
        return;

    /* Find '!' command terminator */
    char *bang = (char *)memchr(p->cmd_buf, '!', p->cmd_len);
    if (bang) {
        size_t cmd_end = (size_t)(bang - p->cmd_buf);
        /* Process through sensor (without '!') */
        sdi12_sensor_process(&p->sensor, p->cmd_buf, cmd_end);
    }

    p->cmd_len = 0;
}

static size_t lb_read(hal_t *self, char *buf, size_t max, uint32_t timeout_ms) {
    loopback_priv_t *p = (loopback_priv_t *)self->priv;

    size_t n = lb_ring_get(&p->resp, buf, max);

    /* If no data available, sleep briefly to avoid CPU spin in timeout loops */
    if (n == 0 && timeout_ms > 0) {
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
    }

    return n;
}

static void lb_set_break(hal_t *self, bool enable) {
    loopback_priv_t *p = (loopback_priv_t *)self->priv;

    if (p->break_active && !enable) {
        /* Falling edge: break just ended — notify sensor */
        sdi12_sensor_break(&p->sensor);

        /* Drain any stale data from the response buffer */
        p->resp.head = 0;
        p->resp.tail = 0;
    }

    p->break_active = enable;
}

static void lb_set_rts(hal_t *self, bool high) {
    (void)self;
    (void)high;
}

/* ── Platform Timing ──────────────────────────────────────────── */

#ifdef _WIN32

static uint64_t lb_micros(hal_t *self) {
    (void)self;
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)(count.QuadPart * 1000000 / freq.QuadPart);
}

static void lb_delay_ms(hal_t *self, uint32_t ms) {
    (void)self;
    (void)ms;
    Sleep(0);   /* yield CPU slice, near-zero delay */
}

static void lb_delay_us(hal_t *self, uint64_t us) {
    (void)self;
    (void)us;
}

#else  /* POSIX */

static uint64_t lb_micros(hal_t *self) {
    (void)self;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static void lb_delay_ms(hal_t *self, uint32_t ms) {
    (void)self;
    (void)ms;
    usleep(10);   /* minimal delay — just enough to advance micros() */
}

static void lb_delay_us(hal_t *self, uint64_t us) {
    (void)self;
    (void)us;
}

#endif

/* ── Factory ──────────────────────────────────────────────────── */

hal_t *hal_create_loopback(char addr) {
    hal_t *h = (hal_t *)calloc(1, sizeof(hal_t));
    if (!h) return NULL;

    loopback_priv_t *p = (loopback_priv_t *)calloc(1, sizeof(loopback_priv_t));
    if (!p) { free(h); return NULL; }

    p->addr = addr;

    h->open      = lb_open;
    h->close     = lb_close;
    h->write     = lb_write;
    h->read      = lb_read;
    h->flush     = lb_flush;
    h->set_break = lb_set_break;
    h->set_rts   = lb_set_rts;
    h->micros    = lb_micros;
    h->delay_ms  = lb_delay_ms;
    h->delay_us  = lb_delay_us;
    h->priv      = p;

    return h;
}
