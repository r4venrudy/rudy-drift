#include "data.h"
#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    puts(
        "Rudy Drift " RD_VERSION " - calibrated network anomaly triage\n"
        "  rudy-drift zeek conn.log\n"
        "  rudy-drift fit train.csv calibration.csv model.rdm [--trees N] [--sample N] [--seed N]\n"
        "  rudy-drift score model.rdm observations.csv [--alpha P]\n"
        "  rudy-drift evidence model.rdm observations.csv [--alpha P]\n"
        "  rudy-drift drift model.rdm observations.csv\n"
        "All reports go to stdout. Fit refuses to overwrite files. See docs/usage.md.");
}
static int integer(const char *s, uint64_t *out) {
    if (!*s)
        return 0;
    for (const char *p = s; *p; ++p)
        if (*p < '0' || *p > '9')
            return 0;
    errno = 0;
    char *end;
    unsigned long long n = strtoull(s, &end, 10);
    if (errno || *end)
        return 0;
    *out = (uint64_t)n;
    return 1;
}
static int probability(const char *s, double *out) {
    if (!*s || *s == ' ' || *s == '\t')
        return 0;
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || *end || !isfinite(v) || v <= 0 || v >= 1)
        return 0;
    *out = v;
    return 1;
}
static int schemas(const dataset *a, const dataset *b) {
    if (a->features != b->features)
        return 0;
    for (unsigned j = 0; j < a->features; ++j)
        if (strcmp(a->names[j], b->names[j]))
            return 0;
    return 1;
}
static int fit(int argc, char **argv) {
    rd_config cfg = {128, 256, 42};
    int sample_given = 0;
    unsigned flags = 0;
    for (int i = 5; i < argc; i += 2) {
        uint64_t n;
        if (i + 1 >= argc || !integer(argv[i + 1], &n)) {
            fputs("invalid fit option/value\n", stderr);
            return 2;
        }
        unsigned flag;
        if (!strcmp(argv[i], "--trees") && n >= 1 && n <= 512) {
            cfg.trees = (unsigned)n;
            flag = 1;
        } else if (!strcmp(argv[i], "--sample") && n >= 2 && n <= 4096) {
            cfg.sample_size = (unsigned)n;
            sample_given = 1;
            flag = 2;
        } else if (!strcmp(argv[i], "--seed")) {
            cfg.seed = n;
            flag = 4;
        } else {
            fputs("unknown or out-of-range fit option\n", stderr);
            return 2;
        }
        if (flags & flag) {
            fputs("duplicate fit option\n", stderr);
            return 2;
        }
        flags |= flag;
    }
    dataset train = {0}, cal = {0};
    char e[RD_ERROR_SIZE];
    int rc = 1;
    if (!data_read(argv[2], &train, RD_MAX_ROWS, e)) {
        fprintf(stderr, "%s\n", e);
        return 1;
    }
    if (!data_read(argv[3], &cal, RD_MAX_CALIBRATION, e)) {
        fprintf(stderr, "%s\n", e);
        goto done;
    }
    if (!schemas(&train, &cal)) {
        fputs("training/calibration schema mismatch\n", stderr);
        goto done;
    }
    if (!strcmp(argv[2], argv[3]) ||
        (train.rows == cal.rows &&
         !memcmp(train.values, cal.values, train.rows * train.features * sizeof(double)))) {
        fputs("training and calibration must be separate samples\n", stderr);
        goto done;
    }
    if (!sample_given && train.rows < cfg.sample_size)
        cfg.sample_size = (unsigned)train.rows;
    const char *names[RD_MAX_FEATURES];
    for (unsigned j = 0; j < train.features; ++j)
        names[j] = train.names[j];
    rd_model *m =
        rd_fit(train.values, train.rows, cal.values, cal.rows, train.features, names, cfg, e);
    if (!m) {
        fprintf(stderr, "%s\n", e);
        goto done;
    }
    FILE *f = fopen(argv[4], "wbx");
    if (!f) {
        fputs("cannot create model (existing paths are never overwritten)\n", stderr);
        rd_free(m);
        goto done;
    }
    int saved = rd_save(m, f, e);
    int closed = fclose(f);
    if (!saved || closed) {
        fputs("model write failed; discard the partial file\n", stderr);
        rd_free(m);
        goto done;
    }
    printf("{\"version\":\"" RD_VERSION
           "\",\"training_rows\":%zu,\"calibration_rows\":%zu,\"features\":%u,\"trees\":%u,"
           "\"sample_size\":%u,\"seed\":\"%" PRIu64 "\"}\n",
           train.rows, cal.rows, train.features, cfg.trees, cfg.sample_size, cfg.seed);
    rd_free(m);
    rc = 0;
done:
    data_free(&train);
    data_free(&cal);
    return rc;
}
static void explanations(const rd_model *m, const double *row) {
    double d[RD_MAX_FEATURES];
    (void)rd_explain(m, row, d);
    fputs("[", stdout);
    for (unsigned j = 0; j < rd_features(m); ++j) {
        if (j)
            putchar(',');
        fputs("{\"feature\":", stdout);
        json_string(stdout, rd_feature_name(m, j));
        printf(",\"value\":%.17g,\"training_median\":%.17g,\"score_delta\":%.17g}", row[j],
               rd_median(m, j), d[j]);
    }
    putchar(']');
}
static int report(int argc, char **argv) {
    int is_drift = !strcmp(argv[1], "drift"), evidence = !strcmp(argv[1], "evidence");
    double alpha = 0.05;
    if (argc != 4 &&
        (is_drift || argc != 6 || strcmp(argv[4], "--alpha") || !probability(argv[5], &alpha))) {
        fputs("invalid report options\n", stderr);
        return 2;
    }
    char e[RD_ERROR_SIZE];
    FILE *f = fopen(argv[2], "rb");
    if (!f) {
        fputs("cannot open model\n", stderr);
        return 1;
    }
    rd_model *m = rd_load(f, e);
    fclose(f);
    if (!m) {
        fprintf(stderr, "%s\n", e);
        return 1;
    }
    dataset data = {0};
    int rc = 1;
    if (!data_read(argv[3], &data, RD_MAX_ROWS, e)) {
        fprintf(stderr, "%s\n", e);
        goto done;
    }
    if (data.features != rd_features(m)) {
        fputs("model/input schema mismatch\n", stderr);
        goto done;
    }
    for (unsigned j = 0; j < data.features; ++j)
        if (strcmp(data.names[j], rd_feature_name(m, j))) {
            fputs("model/input schema mismatch (names and order must match)\n", stderr);
            goto done;
        }
    if (!is_drift && alpha < 1.0 / (double)(rd_calibration_rows(m) + 1))
        fprintf(stderr, "warning: alpha is below calibration resolution; no alerts are possible\n");
    if (is_drift) {
        double *values = (double *)malloc(data.rows * sizeof(double));
        if (!values) {
            fputs("out of memory\n", stderr);
            goto done;
        }
        printf("{\"rows\":%zu,\"reference_rows\":%zu,\"features\":[", data.rows,
               rd_calibration_rows(m));
        for (unsigned j = 0; j < data.features; ++j) {
            for (size_t i = 0; i < data.rows; ++i)
                values[i] = data.values[i * data.features + j];
            double distance;
            if (!rd_ks(m, j, values, data.rows, &distance)) {
                free(values);
                fputs("drift calculation failed; discard partial output\n", stderr);
                goto done;
            }
            if (j)
                putchar(',');
            fputs("{\"feature\":", stdout);
            json_string(stdout, data.names[j]);
            printf(",\"ks_distance\":%.17g}", distance);
        }
        fputs("]}\n", stdout);
        free(values);
    } else {
        if (evidence)
            fputs("{\"findings\":[", stdout);
        size_t emitted = 0;
        for (size_t i = 0; i < data.rows; ++i) {
            const double *row = data.values + i * data.features;
            rd_prediction p;
            (void)rd_predict(m, row, &p);
            int alert = p.p_value <= alpha;
            if (evidence) {
                if (!alert)
                    continue;
                if (emitted++)
                    putchar(',');
                fputs("{\"producer\":\"rudy-drift\",\"rule_id\":\"DRIFT.ANOMALY.001\",\"title\":"
                      "\"Unusual network observation: review "
                      "required\",\"severity\":\"info\",\"asset\":{\"kind\":\"other\",\"id\":",
                      stdout);
                json_string(stdout, data.ids[i]);
                printf("},\"evidence\":[\"Isolation Forest score %.8g; empirical tail p-value "
                       "%.8g; threshold %.8g. An anomaly is not proof of malicious behavior.\"",
                       p.score, p.p_value, alpha);
                double d[RD_MAX_FEATURES];
                (void)rd_explain(m, row, d);
                for (unsigned j = 0; j < data.features; ++j)
                    printf(",\"%s: value %.8g; training median %.8g; median-substitution score "
                           "delta %.8g\"",
                           data.names[j], row[j], rd_median(m, j), d[j]);
                fputs("]}", stdout);
            } else {
                fputs("{\"id\":", stdout);
                json_string(stdout, data.ids[i]);
                printf(",\"score\":%.17g,\"p_value\":%.17g,\"alpha\":%.17g,\"alert\":%s,"
                       "\"sensitivity\":",
                       p.score, p.p_value, alpha, alert ? "true" : "false");
                if (alert)
                    explanations(m, row);
                else
                    fputs("[]", stdout);
                fputs("}\n", stdout);
            }
        }
        if (evidence)
            fputs("]}\n", stdout);
    }
    rc = 0;
done:
    data_free(&data);
    rd_free(m);
    return rc;
}
int main(int argc, char **argv) {
    setlocale(LC_NUMERIC, "C");
    int rc;
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        // one line, so scripts and container health checks can read it
        puts("rudy-drift " RD_VERSION);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        usage();
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "zeek")) {
        dataset d;
        char e[RD_ERROR_SIZE];
        if (!zeek_read(argv[2], &d, e)) {
            fprintf(stderr, "%s\n", e);
            return 1;
        }
        data_csv(&d, stdout);
        data_free(&d);
        rc = 0;
    } else if (argc >= 5 && !strcmp(argv[1], "fit"))
        rc = fit(argc, argv);
    else if (argc >= 4 && (!strcmp(argv[1], "score") || !strcmp(argv[1], "evidence") ||
                           !strcmp(argv[1], "drift")))
        rc = report(argc, argv);
    else {
        usage();
        return 2;
    }
    if (fflush(stdout) || ferror(stdout)) {
        fputs("output write failed\n", stderr);
        return 1;
    }
    return rc;
}
