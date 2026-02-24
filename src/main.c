/**
 * @file main.c
 * @brief CLI entry point and verifier lifecycle.
 *
 * libsdi12-verifier — Open-Source SDI-12 Compliance Tester
 * https://github.com/phillipweinstock/libsdi12-verifier
 */
#include "verifier.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERSION "0.6.0"

/* Exit codes — CI-friendly */
#define EXIT_PASS   0   /**< All tests passed                        */
#define EXIT_FAIL   1   /**< One or more tests failed                */
#define EXIT_ERROR  2   /**< Internal error (bad args, port, etc.)   */

/* ══════════════════════════════════════════════════════════════════════
 *  CLI
 * ══════════════════════════════════════════════════════════════════════ */

static void print_banner(void) {
#ifdef _WIN32
    printf(
        "\n"
        "  +------------------------------------------------------+\n"
        "  |            libsdi12-verifier v" VERSION "                  |\n"
        "  |            Open-Source SDI-12 Compliance Tester      |\n"
        "  |            Built on libsdi12 -- MIT License          |\n"
        "  +------------------------------------------------------+\n"
        "\n"
    );
#else
    printf(
        "\n"
        "  ┌──────────────────────────────────────────────────────┐\n"
        "  │            libsdi12-verifier v" VERSION "                  │\n"
        "  │            Open-Source SDI-12 Compliance Tester      │\n"
        "  │            Built on libsdi12 — MIT License           │\n"
        "  └──────────────────────────────────────────────────────┘\n"
        "\n"
    );
#endif
}

static void print_usage(const char *prog) {
    print_banner();
    printf(
        "Usage:\n"
        "  %s --port <port> [options]\n"
        "\n"
        "Modes:\n"
        "  --test-sensor         Test a real SDI-12 sensor (default)\n"
        "  --test-recorder       Test a data recorder (simulated sensor)\n"
        "  --self-test           Loopback self-test (no hardware needed)\n"
        "  --monitor             Passive bus monitor / sniffer\n"
        "  --transparent         Interactive command mode (send/receive)\n"
        "\n"
        "Options:\n"
        "  --port <port>    -p   Serial port (e.g. COM3, /dev/ttyUSB0)\n"
        "  --addr <a>       -a   Sensor address, 0-9/A-Z/a-z (default: 0)\n"
        "  --test <name>    -t   Run only matching test(s) by name\n"
        "  --format <fmt>   -f   Output: text, json (default: text)\n"
        "  --output <file>  -o   Output file (default: stdout)\n"
        "  --rts                 Use RTS line for TX/RX direction control\n"
        "  --hex                 Show raw hex bytes in monitor mode\n"
        "  --color               Force colored output\n"
        "  --no-color            Disable colored output\n"
        "  --verbose        -v   Verbose output during testing\n"
        "  --help           -h   Show this help\n"
        "  --version             Show version\n"
        "\n"
        "Examples:\n"
        "  %s --port COM3 --test-sensor --addr 0\n"
        "  %s --port /dev/ttyUSB0 --test-sensor -f json -o report.json\n"
        "  %s --port COM3 --transparent\n"
        "  %s --port COM3 --test-sensor --test acknowledge\n"
        "  %s --port COM3 --monitor\n"
        "\n",
        prog, prog, prog, prog, prog, prog
    );
}

static int parse_args(int argc, char **argv, verifier_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->addr        = '0';
    ctx->mode        = MODE_SENSOR_TEST;
    ctx->format      = REPORT_TEXT;
    ctx->use_color   = true;   /* default: color on if stdout is a tty */
    ctx->test_filter = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("libsdi12-verifier v" VERSION "\n");
            exit(0);
        }

        if (strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --port requires argument\n"); return -1; }
            ctx->port = argv[i];
        }
        else if (strcmp(argv[i], "--addr") == 0 || strcmp(argv[i], "-a") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --addr requires argument\n"); return -1; }
            ctx->addr = argv[i][0];
        }
        else if (strcmp(argv[i], "--test") == 0 || strcmp(argv[i], "-t") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --test requires argument\n"); return -1; }
            ctx->test_filter = argv[i];
        }
        else if (strcmp(argv[i], "--format") == 0 || strcmp(argv[i], "-f") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --format requires argument\n"); return -1; }
            if (strcmp(argv[i], "json") == 0) ctx->format = REPORT_JSON;
            else                              ctx->format = REPORT_TEXT;
        }
        else if (strcmp(argv[i], "--output") == 0 || strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --output requires argument\n"); return -1; }
            ctx->output_file = argv[i];
        }
        else if (strcmp(argv[i], "--test-sensor") == 0)    ctx->mode = MODE_SENSOR_TEST;
        else if (strcmp(argv[i], "--test-recorder") == 0)  ctx->mode = MODE_RECORDER_TEST;
        else if (strcmp(argv[i], "--self-test") == 0)       { ctx->mode = MODE_SENSOR_TEST; ctx->self_test = true; }
        else if (strcmp(argv[i], "--monitor") == 0)         ctx->mode = MODE_MONITOR;
        else if (strcmp(argv[i], "--transparent") == 0)     ctx->mode = MODE_TRANSPARENT;
        else if (strcmp(argv[i], "--rts") == 0)             ctx->use_rts = true;
        else if (strcmp(argv[i], "--hex") == 0)             ctx->hex_monitor = true;
        else if (strcmp(argv[i], "--color") == 0)           ctx->use_color = true;
        else if (strcmp(argv[i], "--no-color") == 0)        ctx->use_color = false;
        else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0)
            ctx->verbose = true;
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }

    if (!ctx->port && !ctx->self_test) {
        fprintf(stderr, "Error: --port is required\n\n");
        print_usage(argv[0]);
        return -1;
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Verifier Lifecycle
 * ══════════════════════════════════════════════════════════════════════ */

int verifier_init(verifier_ctx_t *ctx) {
    if (ctx->self_test) {
        ctx->hal = hal_create_loopback(ctx->addr);
        if (!ctx->hal) {
            fprintf(stderr, "Error: failed to create loopback HAL\n");
            return -1;
        }
        /* Open with dummy config — loopback ignores serial settings */
        hal_serial_config_t cfg = hal_sdi12_default("loopback");
        if (ctx->hal->open(ctx->hal, &cfg) != 0) {
            fprintf(stderr, "Error: loopback sensor init failed\n");
            hal_destroy(ctx->hal);
            ctx->hal = NULL;
            return -1;
        }
    } else {
        ctx->hal = hal_create_default();
        if (!ctx->hal) {
            fprintf(stderr, "Error: failed to create platform HAL\n");
            return -1;
        }
        hal_serial_config_t cfg = hal_sdi12_default(ctx->port);
        if (ctx->hal->open(ctx->hal, &cfg) != 0) {
            fprintf(stderr, "Error: cannot open %s\n", ctx->port);
            hal_destroy(ctx->hal);
            ctx->hal = NULL;
            return -1;
        }
    }

    if (timing_init(&ctx->timing, ctx->hal, ctx->use_rts) != 0) {
        fprintf(stderr, "Error: timing layer init failed\n");
        ctx->hal->close(ctx->hal);
        hal_destroy(ctx->hal);
        ctx->hal = NULL;
        return -1;
    }

    const char *mode_name =
        ctx->self_test                  ? "Self-Test (Loopback)" :
        ctx->mode == MODE_SENSOR_TEST   ? "Sensor Compliance" :
        ctx->mode == MODE_RECORDER_TEST ? "Recorder Compliance" :
        ctx->mode == MODE_TRANSPARENT   ? "Transparent" :
                                          "Bus Monitor";
    const char *port_name = ctx->self_test ? "loopback" : ctx->port;
    report_init(&ctx->report, mode_name, port_name, ctx->addr);
    ctx->report.use_color = ctx->use_color;

    return 0;
}

int verifier_run(verifier_ctx_t *ctx) {
    switch (ctx->mode) {
        case MODE_SENSOR_TEST:    return verifier_run_sensor_tests(ctx);
        case MODE_RECORDER_TEST:  return verifier_run_recorder_tests(ctx);
        case MODE_MONITOR:        return verifier_run_monitor(ctx);
        case MODE_TRANSPARENT:    return verifier_run_transparent(ctx);
    }
    return -1;
}

void verifier_cleanup(verifier_ctx_t *ctx) {
    if (ctx->hal) {
        ctx->hal->close(ctx->hal);
        hal_destroy(ctx->hal);
        ctx->hal = NULL;
    }
}

int verifier_run_sensor_tests(verifier_ctx_t *ctx) {
    test_suite_t suite;
    memset(&suite, 0, sizeof(suite));
    suite.name = "SDI-12 Sensor Compliance";
    sensor_tests_register(&suite);

    /* Pre-scan: identify the sensor and store in report */
    sdi12_master_send_break(timing_master(&ctx->timing));
    sdi12_ident_t ident;
    memset(&ident, 0, sizeof(ident));
    if (sdi12_master_identify(timing_master(&ctx->timing), ctx->addr, &ident) == SDI12_OK) {
        ctx->report.has_ident = true;
        ctx->report.ident     = ident;
    }

    test_result_t results[TEST_MAX_TESTS];
    size_t count = 0;

    if (ctx->verbose) {
        print_banner();
        if (ctx->self_test)
            printf("  Mode:    Self-test (loopback — no hardware)\n");
        else
            printf("  Port:    %s\n", ctx->port);
        printf("  Address: '%c'\n", ctx->addr);
        if (ctx->report.has_ident) {
            printf("  Sensor:  %.8s / %.6s (FW %.3s)\n",
                   ident.vendor, ident.model, ident.firmware_version);
        }
        printf("  Tests:   %zu\n", suite.count);
        if (ctx->test_filter)
            printf("  Filter:  '%s'\n", ctx->test_filter);
        printf("\n");
    }

    test_suite_run(&suite, &ctx->timing, ctx->addr, results, &count,
                   ctx->test_filter);
    report_add_results(&ctx->report, results, count);

    size_t pass, fail, skip, error;
    report_summary(&ctx->report, &pass, &fail, &skip, &error);

    if (error > 0) return EXIT_ERROR;
    if (fail  > 0) return EXIT_FAIL;
    return EXIT_PASS;
}

int verifier_run_recorder_tests(verifier_ctx_t *ctx) {
    test_suite_t suite;
    memset(&suite, 0, sizeof(suite));
    suite.name = "SDI-12 Recorder Compliance";
    recorder_tests_register(&suite);

    test_result_t results[TEST_MAX_TESTS];
    size_t count = 0;

    if (ctx->verbose) {
        print_banner();
        printf("  Mode:    Recorder Test (verifier = simulated sensor)\n");
        printf("  Port:    %s\n", ctx->port);
        printf("  Address: '%c'\n", ctx->addr);
        printf("  Tests:   %zu\n", suite.count);
        printf("\n  Connect the data recorder to %s.\n", ctx->port);
        printf("  The verifier will simulate a sensor at address '%c'.\n\n", ctx->addr);
    }

    test_suite_run(&suite, &ctx->timing, ctx->addr, results, &count,
                   ctx->test_filter);
    report_add_results(&ctx->report, results, count);

    size_t pass, fail, skip, error;
    report_summary(&ctx->report, &pass, &fail, &skip, &error);

    if (error > 0) return EXIT_ERROR;
    if (fail  > 0) return EXIT_FAIL;
    return EXIT_PASS;
}

/* ══════════════════════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    verifier_ctx_t ctx;

    if (parse_args(argc, argv, &ctx) != 0)
        return EXIT_ERROR;

    if (verifier_init(&ctx) != 0)
        return EXIT_ERROR;

    int rc = verifier_run(&ctx);

    /* Output report */
    FILE *out = stdout;
    if (ctx.output_file) {
        out = fopen(ctx.output_file, "w");
        if (!out) {
            fprintf(stderr, "Error: cannot open %s for writing\n", ctx.output_file);
            out = stdout;
        }
    }

    report_print(&ctx.report, ctx.format, out);

    if (out != stdout)
        fclose(out);

    verifier_cleanup(&ctx);
    return rc;
}
