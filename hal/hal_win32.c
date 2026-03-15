/**
 * @file hal_win32.c
 * @brief Windows HAL implementation using Win32 serial API + QPC timing.
 *
 * Serial: CreateFile / DCB / SetCommState / ReadFile / WriteFile
 * Timing: QueryPerformanceCounter (sub-microsecond resolution)
 * Break:  SetCommBreak / ClearCommBreak
 * RTS:    EscapeCommFunction(SETRTS / CLRRTS)
 */
#ifdef _WIN32

#include "hal.h"
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    HANDLE            hComm;
    LARGE_INTEGER     freq;     /* QPC ticks per second */
    hal_serial_config_t cfg;
} win32_priv_t;

/* ── Serial Operations ─────────────────────────────────────────── */

static int win32_open(hal_t *self, const hal_serial_config_t *cfg) {
    win32_priv_t *p = (win32_priv_t *)self->priv;
    p->cfg = *cfg;

    /* Build \\.\COMn path */
    char path[32];
    snprintf(path, sizeof(path), "\\\\.\\%s", cfg->port);

    p->hComm = CreateFileA(path,
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL);

    if (p->hComm == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Error: cannot open %s (error %lu)\n",
                cfg->port, GetLastError());
        return -1;
    }

    /* Configure DCB */
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    GetCommState(p->hComm, &dcb);

    dcb.BaudRate = cfg->baud;
    dcb.ByteSize = cfg->data_bits;
    dcb.Parity   = (cfg->parity == 2) ? EVENPARITY :
                   (cfg->parity == 1) ? ODDPARITY  : NOPARITY;
    dcb.StopBits = (cfg->stop_bits == 2) ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fParity  = (cfg->parity != 0) ? TRUE : FALSE;

    /* Disable flow control */
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl  = DTR_CONTROL_DISABLE;
    dcb.fRtsControl  = RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE;
    dcb.fInX  = FALSE;

    if (!SetCommState(p->hComm, &dcb)) {
        fprintf(stderr, "Error: SetCommState failed (%lu)\n", GetLastError());
        CloseHandle(p->hComm);
        p->hComm = INVALID_HANDLE_VALUE;
        return -1;
    }

    /* Non-blocking read by default (we handle timeouts ourselves) */
    COMMTIMEOUTS timeouts = {
        .ReadIntervalTimeout         = MAXDWORD,
        .ReadTotalTimeoutMultiplier  = 0,
        .ReadTotalTimeoutConstant    = 0,
        .WriteTotalTimeoutMultiplier = 0,
        .WriteTotalTimeoutConstant   = 1000,
    };
    SetCommTimeouts(p->hComm, &timeouts);

    PurgeComm(p->hComm, PURGE_RXCLEAR | PURGE_TXCLEAR);

    return 0;
}

static void win32_close(hal_t *self) {
    win32_priv_t *p = (win32_priv_t *)self->priv;
    if (p->hComm != INVALID_HANDLE_VALUE) {
        CloseHandle(p->hComm);
        p->hComm = INVALID_HANDLE_VALUE;
    }
}

static size_t win32_write(hal_t *self, const char *data, size_t len) {
    win32_priv_t *p = (win32_priv_t *)self->priv;
    DWORD written = 0;
    WriteFile(p->hComm, data, (DWORD)len, &written, NULL);
    return (size_t)written;
}

static size_t win32_read(hal_t *self, char *buf, size_t max, uint32_t timeout_ms) {
    win32_priv_t *p = (win32_priv_t *)self->priv;

    /* Configure timeout for this read */
    COMMTIMEOUTS timeouts = {
        .ReadIntervalTimeout         = MAXDWORD,
        .ReadTotalTimeoutMultiplier  = MAXDWORD,
        .ReadTotalTimeoutConstant    = timeout_ms,
        .WriteTotalTimeoutMultiplier = 0,
        .WriteTotalTimeoutConstant   = 1000,
    };
    SetCommTimeouts(p->hComm, &timeouts);

    DWORD bytes_read = 0;
    ReadFile(p->hComm, buf, (DWORD)max, &bytes_read, NULL);
    return (size_t)bytes_read;
}

static void win32_flush(hal_t *self) {
    win32_priv_t *p = (win32_priv_t *)self->priv;
    FlushFileBuffers(p->hComm);
}

/* ── Bus Control ───────────────────────────────────────────────── */

static void win32_set_break(hal_t *self, bool enable) {
    win32_priv_t *p = (win32_priv_t *)self->priv;
    if (enable)
        SetCommBreak(p->hComm);
    else
        ClearCommBreak(p->hComm);
}

static void win32_set_rts(hal_t *self, bool high) {
    win32_priv_t *p = (win32_priv_t *)self->priv;
    EscapeCommFunction(p->hComm, high ? SETRTS : CLRRTS);
}

/* ── Timing ────────────────────────────────────────────────────── */

static uint64_t win32_micros(hal_t *self) {
    win32_priv_t *p = (win32_priv_t *)self->priv;
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return (uint64_t)(count.QuadPart * 1000000 / p->freq.QuadPart);
}

static void win32_delay_ms(hal_t *self, uint32_t ms) {
    (void)self;
    Sleep(ms);
}

static void win32_delay_us(hal_t *self, uint64_t us) {
    /* Spin-wait for microsecond precision */
    uint64_t target = win32_micros(self) + us;
    while (win32_micros(self) < target)
        ;  /* busy wait */
}

/* ── Factory ───────────────────────────────────────────────────── */

hal_t *hal_create_default(void) {
    hal_t        *h = (hal_t *)calloc(1, sizeof(hal_t));
    win32_priv_t *p = (win32_priv_t *)calloc(1, sizeof(win32_priv_t));

    if (!h || !p) {
        free(h);
        free(p);
        return NULL;
    }

    p->hComm = INVALID_HANDLE_VALUE;
    QueryPerformanceFrequency(&p->freq);

    h->open      = win32_open;
    h->close     = win32_close;
    h->write     = win32_write;
    h->read      = win32_read;
    h->flush     = win32_flush;
    h->set_break = win32_set_break;
    h->set_rts   = win32_set_rts;
    h->micros    = win32_micros;
    h->delay_ms  = win32_delay_ms;
    h->delay_us  = win32_delay_us;
    h->priv      = p;

    return h;
}

void hal_destroy(hal_t *hal) {
    if (hal) {
        free(hal->priv);
        free(hal);
    }
}

#endif /* _WIN32 */
