/**
 * @file transparent.c
 * @brief Interactive transparent mode — send commands, see raw responses.
 *
 * A REPL that lets the user type SDI-12 commands and see the raw bus
 * responses.  Sends a break before each command.  Useful for debugging
 * sensors without writing test code.
 *
 * Usage: sdi12-verifier --port COM3 --transparent --addr 0
 *
 * At the prompt the user can type:
 *   - A bare command body (e.g. "I!" or "M!") — address is prepended
 *   - A full command (e.g. "0I!") — sent as-is
 *   - "break" — sends a break signal only
 *   - "quit" or Ctrl+C — exits
 */
#include "verifier.h"
#include "sdi12.h"
#include "sdi12_master.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>

static volatile sig_atomic_t transparent_running = 1;

static void transparent_signal(int sig) {
    (void)sig;
    transparent_running = 0;
}

/**
 * Print a raw response with non-printable bytes escaped.
 */
static void print_response(const char *buf, size_t len, uint64_t latency_us) {
    printf("  <- ");
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\r')                        printf("\\r");
        else if (c == '\n')                   printf("\\n");
        else if (c >= 0x20 && c < 0x7F)       printf("%c", c);
        else                                  printf("\\x%02X", (unsigned char)c);
    }
    if (latency_us > 0)
        printf("  (%.3f ms)", latency_us / 1000.0);
    printf("\n");
}

int verifier_run_transparent(verifier_ctx_t *ctx) {
    printf("\n");
    printf("  ================================================================\n");
    printf("    SDI-12 Transparent Mode — %s\n", ctx->port);
    printf("    Default address: '%c'\n", ctx->addr);
    printf("    Type a command, 'break', or 'quit'\n");
    printf("    Commands without an address get '%c' prepended\n", ctx->addr);
    printf("  ================================================================\n\n");

    signal(SIGINT, transparent_signal);

    char input[128];

    while (transparent_running) {
        printf("  SDI-12> ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin))
            break;

        /* Strip trailing newline/whitespace */
        size_t len = strlen(input);
        while (len > 0 && (input[len - 1] == '\n' || input[len - 1] == '\r' ||
                           input[len - 1] == ' '  || input[len - 1] == '\t'))
            input[--len] = '\0';

        if (len == 0)
            continue;

        /* Quit command */
        if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0 ||
            strcmp(input, "q") == 0)
            break;

        /* Break-only command */
        if (strcmp(input, "break") == 0) {
            sdi12_master_send_break(timing_master(&ctx->timing));
            printf("  -- break sent (%.3f ms)\n",
                   timing_break_duration_us(&ctx->timing) / 1000.0);
            continue;
        }

        /* Help */
        if (strcmp(input, "help") == 0 || strcmp(input, "?") == 0) {
            printf("  Commands:\n");
            printf("    I!            Send aI! (address prepended)\n");
            printf("    0I!           Send 0I! (full command, as-is)\n");
            printf("    M!            Send aM! (address prepended)\n");
            printf("    D0!           Send aD0! (address prepended)\n");
            printf("    break         Send break signal only\n");
            printf("    addr <c>      Change default address\n");
            printf("    quit          Exit transparent mode\n\n");
            continue;
        }

        /* Change default address */
        if (strncmp(input, "addr ", 5) == 0 && len >= 6) {
            char new_addr = input[5];
            if (sdi12_valid_address(new_addr)) {
                ctx->addr = new_addr;
                printf("  -- default address changed to '%c'\n", ctx->addr);
            } else {
                printf("  -- invalid address '%c'\n", new_addr);
            }
            continue;
        }

        /* Build the full SDI-12 command */
        char cmd[136];

        if (input[len - 1] != '!') {
            /* Not a valid SDI-12 command — try appending '!' */
            printf("  -- warning: command should end with '!'\n");
        }

        /*
         * If the first character is a valid SDI-12 address or '?',
         * treat it as a full command.  Otherwise prepend the default address.
         */
        if (sdi12_valid_address(input[0]) || input[0] == '?') {
            snprintf(cmd, sizeof(cmd), "%s", input);
        } else {
            snprintf(cmd, sizeof(cmd), "%c%s", ctx->addr, input);
        }

        /* Send break + command */
        sdi12_master_send_break(timing_master(&ctx->timing));

        printf("  -> %s\n", cmd);
        fflush(stdout);

        /* Use transact for the raw send/recv */
        sdi12_err_t err = sdi12_master_transact(
            timing_master(&ctx->timing), cmd, 1000);

        if (err == SDI12_ERR_TIMEOUT) {
            printf("  <- (no response)\n");
        } else if (err != SDI12_OK) {
            printf("  <- (error %d)\n", err);
        } else {
            /* Print the raw response from the master context */
            sdi12_master_ctx_t *m = timing_master(&ctx->timing);
            print_response(m->resp_buf, m->resp_len,
                           timing_response_latency_us(&ctx->timing));
        }
    }

    printf("\n  Transparent mode stopped.\n");
    return 0;
}
