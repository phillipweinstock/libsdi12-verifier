/**
 * @file monitor.c
 * @brief Passive SDI-12 bus monitor / sniffer with timestamped decode.
 *
 * Supports two display modes:
 *   - Default:  printable chars with \r \n \xNN escapes
 *   - Hex:      raw hex byte dump (--hex flag)
 */
#include "verifier.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>

static volatile sig_atomic_t monitor_running = 1;

static void monitor_signal(int sig) {
    (void)sig;
    monitor_running = 0;
}

int verifier_run_monitor(verifier_ctx_t *ctx) {
    const bool hex_mode = ctx->hex_monitor;

    printf("\n");
    printf("  ================================================================\n");
    printf("    SDI-12 Bus Monitor — %s%s\n",
           ctx->port, hex_mode ? " [HEX]" : "");
    printf("    Press Ctrl+C to stop\n");
    printf("  ================================================================\n\n");
    printf("  %-14s  %-4s  %s\n", "TIMESTAMP", "DIR", "DATA");
    printf("  --------------  ----  ----------------------------------------\n");

    signal(SIGINT, monitor_signal);

    uint64_t start_us = ctx->hal->micros(ctx->hal);
    char     line_buf[128];
    size_t   line_pos = 0;

    while (monitor_running) {
        char   c;
        size_t n = ctx->hal->read(ctx->hal, &c, 1, 100);

        if (n == 1) {
            if (line_pos < sizeof(line_buf) - 1)
                line_buf[line_pos++] = c;

            /* Print on newline or buffer full */
            if (c == '\n' || line_pos >= sizeof(line_buf) - 1) {
                uint64_t elapsed = ctx->hal->micros(ctx->hal) - start_us;
                double   secs    = elapsed / 1000000.0;

                printf("  %10.3f s    RX    ", secs);

                if (hex_mode) {
                    for (size_t i = 0; i < line_pos; i++)
                        printf("%02X ", (unsigned char)line_buf[i]);
                } else {
                    for (size_t i = 0; i < line_pos; i++) {
                        char ch = line_buf[i];
                        if (ch == '\r')                          printf("\\r");
                        else if (ch == '\n')                     printf("\\n");
                        else if (ch >= 0x20 && ch < 0x7F)        printf("%c", ch);
                        else                                     printf("\\x%02X", (unsigned char)ch);
                    }
                }
                printf("\n");
                fflush(stdout);

                line_pos = 0;
            }
        }
    }

    /* Flush any remaining partial line */
    if (line_pos > 0) {
        uint64_t elapsed = ctx->hal->micros(ctx->hal) - start_us;
        printf("  %10.3f s    RX    ", elapsed / 1000000.0);
        if (hex_mode) {
            for (size_t i = 0; i < line_pos; i++)
                printf("%02X ", (unsigned char)line_buf[i]);
        } else {
            for (size_t i = 0; i < line_pos; i++) {
                char ch = line_buf[i];
                if (ch >= 0x20 && ch < 0x7F) printf("%c", ch);
                else                          printf("\\x%02X", (unsigned char)ch);
            }
        }
        printf("\n");
    }

    printf("\n  Monitor stopped.\n");
    return 0;
}
