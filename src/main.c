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

#define VERSION "0.1.0"

/* ══════════════════════════════════════════════════════════════════════
 *  CLI
 * ══════════════════════════════════════════════════════════════════════ */

static void print_banner(void) {
    printf(
        "\n"
        "  ┌──────────────────────────────────────────────────────┐\n"
        "  │  libsdi12-verifier v" VERSION "                          │\n"
        "  │  Open-Source SDI-12 Compliance Tester                │\n"
        "  │  Built on libsdi12 — MIT License                    │\n"
        "  └──────────────────────────────────────────────────────┘\n"
        "\n"
    );
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
        "  --monitor             Passive bus monitor / sniffer\n"
        "\n"
        "Options:\n"
        "  --port <port>    -p   Serial port (e.g. COM3, /dev/ttyUSB0)\n"
        "  --addr <a>       -a   Sensor address, 0-9/A-Z/a-z (default: 0)\n"
        "  --format <fmt>   -f   Output: text, json (default: text)\n"
        "  --output <file>  -o   Output file (default: stdout)\n"
        "  --rts                 Use RTS line for TX/RX direction control\n"
        "  --verbose        -v   Verbose output during testing\n"
        "  --help           -h   Show this help\n"
        "  --version             Show version\n"
        "\n"
        "Examples:\n"
        "  %s --port COM3 --test-sensor --addr 0\n"
        "  %s --port /dev/ttyUSB0 --test-sensor -f json -o report.json\n"
        "  %s --port COM3 --monitor\n"
        "\n",
        prog, prog, prog, prog
    );
}

static int parse_args(int argc, char **argv, verifier_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->addr   = '0';
    ctx->mode   = MODE_SENSOR_TEST;
    ctx->format = REPORT_TEXT;

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
        else if (strcmp(argv[i], "--monitor") == 0)         ctx->mode = MODE_MONITOR;
        else if (strcmp(argv[i], "--rts") == 0)             ctx->use_rts = true;
        else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0)
            ctx->verbose = true;
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }

    if (!ctx->port) {
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

    if (timing_init(&ctx->timing, ctx->hal, ctx->use_rts) != 0) {
        fprintf(stderr, "Error: timing layer init failed\n");
        ctx->hal->close(ctx->hal);
        hal_destroy(ctx->hal);
        ctx->hal = NULL;
        return -1;
    }

    const char *mode_name =
        ctx->mode == MODE_SENSOR_TEST   ? "Sensor Compliance" :
        ctx->mode == MODE_RECORDER_TEST ? "Recorder Compliance" :
                                          "Bus Monitor";
    report_init(&ctx->report, mode_name, ctx->port, ctx->addr);

    return 0;
}

int verifier_run(verifier_ctx_t *ctx) {
    switch (ctx->mode) {
        case MODE_SENSOR_TEST:    return verifier_run_sensor_tests(ctx);
        case MODE_RECORDER_TEST:  return verifier_run_recorder_tests(ctx);
        case MODE_MONITOR:        return verifier_run_monitor(ctx);
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

    test_result_t results[TEST_MAX_TESTS];
    size_t count = 0;

    if (ctx->verbose) {
        print_banner();
        printf("  Port:    %s\n", ctx->port);
        printf("  Address: '%c'\n", ctx->addr);
        printf("  Tests:   %zu\n\n", suite.count);
    }

    test_suite_run(&suite, &ctx->timing, ctx->addr, results, &count);
    report_add_results(&ctx->report, results, count);

    size_t pass, fail, skip, error;
    report_summary(&ctx->report, &pass, &fail, &skip, &error);

    return (fail > 0 || error > 0) ? 1 : 0;
}

int verifier_run_recorder_tests(verifier_ctx_t *ctx) {
    test_suite_t suite;
    memset(&suite, 0, sizeof(suite));
    suite.name = "SDI-12 Recorder Compliance";
    recorder_tests_register(&suite);

    if (suite.count == 0) {
        printf("Recorder tests not yet implemented.\n");
        return 0;
    }

    test_result_t results[TEST_MAX_TESTS];
    size_t count = 0;

    test_suite_run(&suite, &ctx->timing, ctx->addr, results, &count);
    report_add_results(&ctx->report, results, count);

    size_t pass, fail, skip, error;
    report_summary(&ctx->report, &pass, &fail, &skip, &error);

    return (fail > 0 || error > 0) ? 1 : 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    verifier_ctx_t ctx;

    if (parse_args(argc, argv, &ctx) != 0)
        return 1;

    if (verifier_init(&ctx) != 0)
        return 1;

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
