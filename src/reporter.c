/**
 * @file reporter.c
 * @brief Compliance report generation — text and JSON.
 */
#include "reporter.h"
#include <string.h>
#include <time.h>

void report_init(report_t *rpt, const char *suite, const char *port, char addr) {
    memset(rpt, 0, sizeof(*rpt));
    rpt->suite_name    = suite;
    rpt->device_port   = port;
    rpt->device_addr   = addr;
    rpt->sdi12_version = "1.4";
}

void report_add_result(report_t *rpt, const test_result_t *result) {
    size_t max = sizeof(rpt->results) / sizeof(rpt->results[0]);
    if (rpt->count < max)
        rpt->results[rpt->count++] = *result;
}

void report_add_results(report_t *rpt, const test_result_t *results, size_t count) {
    for (size_t i = 0; i < count; i++)
        report_add_result(rpt, &results[i]);
}

void report_summary(const report_t *rpt, size_t *pass, size_t *fail,
                    size_t *skip, size_t *error) {
    *pass = *fail = *skip = *error = 0;
    for (size_t i = 0; i < rpt->count; i++) {
        switch (rpt->results[i].status) {
            case TEST_PASS:  (*pass)++;  break;
            case TEST_FAIL:  (*fail)++;  break;
            case TEST_SKIP:  (*skip)++;  break;
            case TEST_ERROR: (*error)++; break;
        }
    }
}

/* ── Helpers ───────────────────────────────────────────────────────── */

static const char *status_str(test_status_t s) {
    switch (s) {
        case TEST_PASS:  return "PASS";
        case TEST_FAIL:  return "FAIL";
        case TEST_SKIP:  return "SKIP";
        case TEST_ERROR: return "ERROR";
    }
    return "?";
}

static void json_str(FILE *out, const char *s) {
    fputc('"', out);
    if (s) {
        for (; *s; s++) {
            switch (*s) {
                case '"':  fputs("\\\"", out); break;
                case '\\': fputs("\\\\", out); break;
                case '\n': fputs("\\n", out);  break;
                case '\r': fputs("\\r", out);  break;
                case '\t': fputs("\\t", out);  break;
                default:   fputc(*s, out);     break;
            }
        }
    }
    fputc('"', out);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Text Report
 * ══════════════════════════════════════════════════════════════════════ */

void report_print_text(const report_t *rpt, FILE *out) {
    time_t now = time(NULL);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    fprintf(out, "\n");
    fprintf(out, "================================================================\n");
    fprintf(out, "  SDI-12 Compliance Report\n");
    fprintf(out, "  Generated: %s\n", timebuf);
    fprintf(out, "  Suite:     %s\n", rpt->suite_name);
    fprintf(out, "  Port:      %s\n", rpt->device_port);
    fprintf(out, "  Address:   '%c'\n", rpt->device_addr);
    fprintf(out, "  SDI-12:    v%s\n", rpt->sdi12_version);
    fprintf(out, "================================================================\n\n");

    for (size_t i = 0; i < rpt->count; i++) {
        const test_result_t *t = &rpt->results[i];

        fprintf(out, "  [%-5s] %-40s %s\n",
                status_str(t->status),
                t->name ? t->name : "(unnamed)",
                t->spec_section ? t->spec_section : "");

        if (t->detail[0])
            fprintf(out, "          %s\n", t->detail);

        if (t->measured_us > 0) {
            fprintf(out, "          Measured: %.3f ms", t->measured_us / 1000.0);
            if (t->spec_limit_us > 0)
                fprintf(out, " (limit: %.3f ms)", t->spec_limit_us / 1000.0);
            fprintf(out, "\n");
        }

        fprintf(out, "\n");
    }

    size_t pass, fail, skip, error;
    report_summary(rpt, &pass, &fail, &skip, &error);

    fprintf(out, "----------------------------------------------------------------\n");
    fprintf(out, "  SUMMARY: %zu passed, %zu failed, %zu skipped, %zu errors\n",
            pass, fail, skip, error);
    fprintf(out, "  RESULT:  %s\n",
            (fail == 0 && error == 0) ? "COMPLIANT" : "NON-COMPLIANT");
    fprintf(out, "================================================================\n");
}

/* ══════════════════════════════════════════════════════════════════════
 *  JSON Report
 * ══════════════════════════════════════════════════════════════════════ */

void report_print_json(const report_t *rpt, FILE *out) {
    time_t now = time(NULL);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S", localtime(&now));

    size_t pass, fail, skip, error;
    report_summary(rpt, &pass, &fail, &skip, &error);

    fprintf(out, "{\n");
    fprintf(out, "  \"tool\": \"libsdi12-verifier\",\n");
    fprintf(out, "  \"version\": \"0.1.0\",\n");
    fprintf(out, "  \"timestamp\": \"%s\",\n", timebuf);
    fprintf(out, "  \"suite\": ");  json_str(out, rpt->suite_name); fprintf(out, ",\n");
    fprintf(out, "  \"port\": ");   json_str(out, rpt->device_port); fprintf(out, ",\n");
    fprintf(out, "  \"address\": \"%c\",\n", rpt->device_addr);
    fprintf(out, "  \"sdi12_version\": \"%s\",\n", rpt->sdi12_version);

    fprintf(out, "  \"summary\": {\n");
    fprintf(out, "    \"total\": %zu,\n", rpt->count);
    fprintf(out, "    \"pass\": %zu,\n", pass);
    fprintf(out, "    \"fail\": %zu,\n", fail);
    fprintf(out, "    \"skip\": %zu,\n", skip);
    fprintf(out, "    \"error\": %zu,\n", error);
    fprintf(out, "    \"compliant\": %s\n", (fail == 0 && error == 0) ? "true" : "false");
    fprintf(out, "  },\n");

    fprintf(out, "  \"tests\": [\n");
    for (size_t i = 0; i < rpt->count; i++) {
        const test_result_t *t = &rpt->results[i];
        fprintf(out, "    {\n");
        fprintf(out, "      \"name\": ");    json_str(out, t->name); fprintf(out, ",\n");
        fprintf(out, "      \"spec\": ");    json_str(out, t->spec_section ? t->spec_section : ""); fprintf(out, ",\n");
        fprintf(out, "      \"status\": \"%s\",\n", status_str(t->status));
        fprintf(out, "      \"detail\": ");  json_str(out, t->detail); fprintf(out, ",\n");
        fprintf(out, "      \"measured_us\": %llu,\n", (unsigned long long)t->measured_us);
        fprintf(out, "      \"spec_limit_us\": %llu\n", (unsigned long long)t->spec_limit_us);
        fprintf(out, "    }%s\n", (i + 1 < rpt->count) ? "," : "");
    }
    fprintf(out, "  ]\n");
    fprintf(out, "}\n");
}

void report_print(const report_t *rpt, report_format_t fmt, FILE *out) {
    switch (fmt) {
        case REPORT_TEXT: report_print_text(rpt, out); break;
        case REPORT_JSON: report_print_json(rpt, out); break;
    }
}
