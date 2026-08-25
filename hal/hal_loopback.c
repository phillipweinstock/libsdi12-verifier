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
#include "hal_loopback.h"
#include "sdi12.h"
#include "sdi12_sensor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

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

/** Simulated sensor values. Nine values so ASCII D responses span more
 *  than one page (9 × "+nn.nn" = 54 chars > the 35-char aM! page limit),
 *  exercising the library's D0/D1 pagination during self-test.
 *  Chosen so that their IEEE-754 float32 representations contain no
 *  0x00 (NUL), 0x0A (LF), or 0x0D (CR) bytes — avoids issues with
 *  strlen-based send and newline-based recv for binary transport.    */
#define LB_PARAM_COUNT 9
static const float LB_VALUES[LB_PARAM_COUNT] = {
    12.34f, 56.78f, 23.45f, 45.67f, 67.89f,
    78.91f, 89.12f, 91.23f, 13.57f
};

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

    /* Active fault-injection mask (lb_fault_t bits) */
    unsigned           faults;

    /* True while the command being processed is an aDBn! request —
     * lets the response path identify binary packets for fault rewrites */
    bool               cmd_is_db;

    /* LB_FAULT_PACED_UART state: byte release schedule for the
     * response currently in the ring */
    bool               pace_active;
    uint64_t           pace_start_us;
    size_t             pace_released;
} loopback_priv_t;

#define LB_PACE_US_PER_BYTE 2000  /* 2 ms per byte while paced */

/* ── Sensor Callbacks ─────────────────────────────────────────── */

static void lb_send_response(const char *data, size_t len, void *user_data) {
    loopback_priv_t *p = (loopback_priv_t *)user_data;

    /* Foreign-sensor fault: re-address aDBn! binary packets to a different
     * sensor address and recompute the CRC so the packet is internally
     * valid — exactly what a misbehaving second sensor on the bus emits.
     * Binary packet layout (§5.2 Table 14):
     *   addr(1) + pkt_size(2 LE) + type(1) + payload(N) + CRC(2 LE)   */
    if ((p->faults & LB_FAULT_FOREIGN_BIN_ADDR) && p->cmd_is_db &&
        len >= 6 && len <= LB_RING_SIZE) {
        char pkt[LB_RING_SIZE];
        memcpy(pkt, data, len);
        pkt[0] = (p->addr == '1') ? '2' : '1';   /* some other address */
        uint16_t crc = sdi12_crc16(pkt, len - 2);
        pkt[len - 2] = (char)(crc & 0xFF);
        pkt[len - 1] = (char)((crc >> 8) & 0xFF);
        lb_ring_put(&p->resp, pkt, len);
        return;
    }

    lb_ring_put(&p->resp, data, len);
}

static sdi12_value_t lb_read_param(uint8_t idx, void *user_data) {
    loopback_priv_t *p = (loopback_priv_t *)user_data;
    sdi12_value_t v;

    /* Hostile-values fault: what a dead ADC / failed conversion returns.
     * A compliant sensor must never let these mis-format on the wire. */
    if (p->faults & LB_FAULT_HOSTILE_VALUES) {
        switch (idx) {
        case 0: v.value = nanf("");   v.decimals = 0; return v;
        case 1: v.value = INFINITY;   v.decimals = 2; return v;
        case 2: v.value = 1e30f;      v.decimals = 0; return v;
        case 3: v.value = -0.0f;      v.decimals = 2; return v;
        default: break;
        }
    }

    /* Boundary-values fault: eight 9-char values ("+12345.67") plus one
     * 3-char value ("+55") = exactly 75 formatted chars, the precise
     * <values> limit of the 75-char page classes. */
    if (p->faults & LB_FAULT_BOUNDARY_VALUES) {
        if (idx < 8) { v.value = 12345.67f; v.decimals = 2; }
        else         { v.value = 55.0f;     v.decimals = 0; }
        return v;
    }

    /* Overwide-value fault: 7 integer digits + 6 requested decimals
     * cannot fit the 9-char cap — the formatter must reduce precision,
     * not truncate into grammar-invalid output. */
    if (p->faults & LB_FAULT_OVERWIDE_VALUE) {
        if (idx == 0) { v.value = 9999999.0f; v.decimals = 6; return v; }
        v.value = 1.0f; v.decimals = 0;
        return v;
    }

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
    loopback_priv_t *pv = (loopback_priv_t *)user_data;

    /* Fat-binary fault: write exactly as much as the documented
     * contract permits — type byte at buf[1] plus payload filling the
     * rest — and embed the payload length in payload[0..1] (LE) so the
     * verifier can detect silent truncation downstream. */
    if (pv && (pv->faults & LB_FAULT_FAT_BINARY)) {
        if (page > 0) return 0;
        if (buflen < 5) return 0;
        size_t payload = buflen - 2;          /* buf[1]=type, rest payload */
        buf[1] = (char)SDI12_BINTYPE_UINT8;
        buf[2] = (char)(payload & 0xFF);
        buf[3] = (char)((payload >> 8) & 0xFF);
        for (size_t i = 4; i < 2 + payload; i++)
            buf[i] = (char)(0x20 + (i & 0x3F));
        return 1 + payload;
    }

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
    cb.read_param    = lb_read_param;
    cb.format_binary_page = lb_format_binary_page;
    cb.user_data     = p;
    /* No start_measurement/service_request → sync measurements (ttt=0) */

    if (sdi12_sensor_init(&p->sensor, p->addr, &ident, &cb) != SDI12_OK)
        return -1;

    /* Register one parameter per simulated value in group 0 (M/C/R) */
    for (int i = 0; i < LB_PARAM_COUNT; i++) {
        sdi12_sensor_register_param(&p->sensor, 0, "TA", "C", 2);
    }

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
        /* Flag aDBn! requests so fault rewrites can target binary packets */
        p->cmd_is_db = (cmd_end >= 3 &&
                        p->cmd_buf[1] == 'D' && p->cmd_buf[2] == 'B');
        /* Process through sensor (without '!') */
        sdi12_sensor_process(&p->sensor, p->cmd_buf, cmd_end);
        p->cmd_is_db = false;
    }

    p->cmd_len = 0;
}

static size_t lb_read(hal_t *self, char *buf, size_t max, uint32_t timeout_ms) {
    loopback_priv_t *p = (loopback_priv_t *)self->priv;

    /* Paced-UART fault: release response bytes on a fixed schedule so
     * the master's recv layer experiences line-rate delivery instead of
     * an instantaneous burst. */
    if (p->faults & LB_FAULT_PACED_UART) {
        size_t avail = lb_ring_avail(&p->resp);
        if (avail == 0) {
            p->pace_active = false;
        } else {
            if (!p->pace_active) {
                p->pace_active   = true;
                p->pace_start_us = self->micros(self);
                p->pace_released = 0;
            }
            uint64_t elapsed = self->micros(self) - p->pace_start_us;
            size_t allowed = (size_t)(elapsed / LB_PACE_US_PER_BYTE) + 1;
            size_t budget  = allowed > p->pace_released
                             ? allowed - p->pace_released : 0;
            if (budget == 0) {
#ifdef _WIN32
                Sleep(1);
#else
                usleep(1000);
#endif
                return 0;
            }
            if (max > budget) max = budget;
            size_t got = lb_ring_get(&p->resp, buf, max);
            p->pace_released += got;
            if (lb_ring_avail(&p->resp) == 0)
                p->pace_active = false;
            return got;
        }
    }

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
    /* Divide before multiplying to avoid int64 overflow (~10 days uptime) */
    uint64_t ticks = (uint64_t)count.QuadPart;
    uint64_t f     = (uint64_t)freq.QuadPart;
    return (ticks / f) * 1000000ULL + (ticks % f) * 1000000ULL / f;
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

void hal_loopback_set_faults(hal_t *hal, unsigned faults) {
    if (!hal || !hal->priv) return;
    loopback_priv_t *p = (loopback_priv_t *)hal->priv;
    p->faults = faults;
}
