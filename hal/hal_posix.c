/**
 * @file hal_posix.c
 * @brief POSIX HAL implementation using termios + clock_gettime.
 *
 * Serial: open() / termios / read() / write() / select()
 * Timing: clock_gettime(CLOCK_MONOTONIC)
 * Break:  ioctl(TIOCSBRK / TIOCCBRK)
 * RTS:    ioctl(TIOCMBIS / TIOCMBIC, TIOCM_RTS)
 */
#ifndef _WIN32

#include "hal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <errno.h>

typedef struct {
    int              fd;
    struct termios   orig;
    hal_serial_config_t cfg;
} posix_priv_t;

/* ── Serial Operations ─────────────────────────────────────────── */

static int posix_open(hal_t *self, const hal_serial_config_t *cfg) {
    posix_priv_t *p = (posix_priv_t *)self->priv;
    p->cfg = *cfg;

    p->fd = open(cfg->port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (p->fd < 0) {
        fprintf(stderr, "Error: cannot open %s: %s\n", cfg->port, strerror(errno));
        return -1;
    }

    /* Save original terminal attributes */
    tcgetattr(p->fd, &p->orig);

    /* Configure: 1200 baud, 7 data bits, even parity, 1 stop bit */
    struct termios tio;
    memset(&tio, 0, sizeof(tio));

    tio.c_cflag = B1200 | CS7 | PARENB | CREAD | CLOCAL;
    tio.c_iflag = INPCK;    /* enable parity checking on input */
    tio.c_oflag = 0;
    tio.c_lflag = 0;        /* raw mode */
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    tcsetattr(p->fd, TCSANOW, &tio);
    tcflush(p->fd, TCIOFLUSH);

    /* Switch to blocking mode (we handle timeouts via select) */
    int flags = fcntl(p->fd, F_GETFL, 0);
    fcntl(p->fd, F_SETFL, flags & ~O_NONBLOCK);

    return 0;
}

static void posix_close(hal_t *self) {
    posix_priv_t *p = (posix_priv_t *)self->priv;
    if (p->fd >= 0) {
        tcsetattr(p->fd, TCSANOW, &p->orig);
        close(p->fd);
        p->fd = -1;
    }
}

static size_t posix_write(hal_t *self, const char *data, size_t len) {
    posix_priv_t *p = (posix_priv_t *)self->priv;
    ssize_t n = write(p->fd, data, len);
    return (n > 0) ? (size_t)n : 0;
}

static size_t posix_read(hal_t *self, char *buf, size_t max, uint32_t timeout_ms) {
    posix_priv_t *p = (posix_priv_t *)self->priv;

    fd_set fds;
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    FD_ZERO(&fds);
    FD_SET(p->fd, &fds);

    int sel = select(p->fd + 1, &fds, NULL, NULL, &tv);
    if (sel <= 0)
        return 0;

    ssize_t n = read(p->fd, buf, max);
    return (n > 0) ? (size_t)n : 0;
}

static void posix_flush(hal_t *self) {
    posix_priv_t *p = (posix_priv_t *)self->priv;
    tcdrain(p->fd);
}

/* ── Bus Control ───────────────────────────────────────────────── */

static void posix_set_break(hal_t *self, bool enable) {
    posix_priv_t *p = (posix_priv_t *)self->priv;
    if (enable)
        ioctl(p->fd, TIOCSBRK);
    else
        ioctl(p->fd, TIOCCBRK);
}

static void posix_set_rts(hal_t *self, bool high) {
    posix_priv_t *p = (posix_priv_t *)self->priv;
    int bits = TIOCM_RTS;
    ioctl(p->fd, high ? TIOCMBIS : TIOCMBIC, &bits);
}

/* ── Timing ────────────────────────────────────────────────────── */

static uint64_t posix_micros(hal_t *self) {
    (void)self;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void posix_delay_ms(hal_t *self, uint32_t ms) {
    (void)self;
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void posix_delay_us(hal_t *self, uint64_t us) {
    (void)self;
    struct timespec ts;
    ts.tv_sec  = (time_t)(us / 1000000ULL);
    ts.tv_nsec = (long)(us % 1000000ULL) * 1000L;
    nanosleep(&ts, NULL);
}

/* ── Factory ───────────────────────────────────────────────────── */

hal_t *hal_create_default(void) {
    hal_t        *h = (hal_t *)calloc(1, sizeof(hal_t));
    posix_priv_t *p = (posix_priv_t *)calloc(1, sizeof(posix_priv_t));

    if (!h || !p) {
        free(h);
        free(p);
        return NULL;
    }

    p->fd = -1;

    h->open      = posix_open;
    h->close     = posix_close;
    h->write     = posix_write;
    h->read      = posix_read;
    h->flush     = posix_flush;
    h->set_break = posix_set_break;
    h->set_rts   = posix_set_rts;
    h->micros    = posix_micros;
    h->delay_ms  = posix_delay_ms;
    h->delay_us  = posix_delay_us;
    h->priv      = p;

    return h;
}

void hal_destroy(hal_t *hal) {
    if (hal) {
        free(hal->priv);
        free(hal);
    }
}

#endif /* !_WIN32 */
