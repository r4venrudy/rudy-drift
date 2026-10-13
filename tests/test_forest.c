#include "rudy_drift.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

int main(void) {
    char error[RD_ERROR_SIZE];
    const char *names[] = {"bytes", "duration"};
    double train[512], cal[256];
    for (unsigned i = 0; i < 256; ++i) {
        train[2 * i] = sin((double)i) * 2;
        train[2 * i + 1] = cos((double)i * 1.73);
    }
    for (unsigned i = 0; i < 128; ++i) {
        cal[2 * i] = sin((double)i + .2) * 2;
        cal[2 * i + 1] = cos((double)i * 1.91);
    }
    rd_config cfg = {64, 128, 42};
    rd_model *m = rd_fit(train, 256, cal, 128, 2, names, cfg, error);
    CHECK(m);
    CHECK(rd_features(m) == 2);
    CHECK(rd_calibration_rows(m) == 128);
    CHECK(!strcmp(rd_feature_name(m, 0), "bytes"));
    CHECK(!rd_feature_name(m, 2));
    CHECK(isnan(rd_median(m, 2)));
    double normal[] = {0, 0}, anomaly[] = {100, 100};
    rd_prediction a, b;
    CHECK(rd_predict(m, normal, &a));
    CHECK(rd_predict(m, anomaly, &b));
    CHECK(b.score > a.score);
    CHECK(b.p_value <= .05);
    CHECK(a.p_value > .05);
    double deltas[2];
    CHECK(rd_explain(m, anomaly, deltas));
    CHECK(deltas[0] > 0 && deltas[1] > 0);
    double restored[] = {rd_median(m, 0), 100};
    rd_prediction c;
    CHECK(rd_predict(m, restored, &c));
    CHECK(fabs(deltas[0] - (b.score - c.score)) < 1e-14);
    double values[128];
    for (unsigned i = 0; i < 128; ++i)
        values[i] = cal[2 * i];
    double ks;
    CHECK(rd_ks(m, 0, values, 128, &ks));
    CHECK(ks == 0);
    for (unsigned i = 0; i < 128; ++i)
        values[i] = 100;
    CHECK(rd_ks(m, 0, values, 128, &ks));
    CHECK(ks == 1);
    FILE *f = tmpfile();
    CHECK(f);
    CHECK(rd_save(m, f, error));
    rewind(f);
    rd_model *copy = rd_load(f, error);
    CHECK(copy);
    CHECK(rd_predict(copy, anomaly, &c));
    CHECK(c.score == b.score && c.p_value == b.p_value);
    rd_free(copy);
    rewind(f);
    CHECK(fputc('X', f) != EOF);
    rewind(f);
    CHECK(!rd_load(f, error));
    fclose(f);
    CHECK(!rd_predict(m, (double[]){NAN, 1}, &c));
    CHECK(!rd_predict(m, (double[]){1, INFINITY}, &c));
    CHECK(!rd_ks(m, 0, (double[]){NAN}, 1, &ks));
    CHECK(!rd_ks(m, 0, values, 0, &ks));
    CHECK(!rd_fit(train, 256, cal, 19, 2, names, cfg, error));
    CHECK(!rd_fit(train, 256, cal, 128, 2, (const char *[]){"x", "x"}, cfg, error));
    CHECK(!rd_fit(train, 256, cal, 128, 2, names, (rd_config){1, 257, 0}, error));
    /* Calibration ties include >=, not >: a constant population never alerts. */
    double flat[64] = {0};
    rd_model *constant = rd_fit(flat, 32, flat, 32, 1, names, (rd_config){8, 32, 1}, error);
    CHECK(constant);
    CHECK(rd_predict(constant, normal, &a));
    CHECK(fabs(a.score - .5) < 1e-14 && a.p_value == 1);
    CHECK(rd_ks(constant, 0, (double[]){0, 0, 1, 1}, 4, &ks));
    CHECK(ks == .5);
    CHECK(rd_explain(constant, normal, deltas));
    CHECK(deltas[0] == 0);
    rd_model *same = rd_fit(train, 256, cal, 128, 2, names, cfg, error);
    CHECK(same);
    CHECK(rd_predict(same, anomaly, &c));
    CHECK(c.score == b.score && c.p_value == b.p_value);
    rd_free(same);
    rd_free(constant);
    rd_free(m);
    rd_free(NULL);
    puts("forest: deterministic scoring, calibration, ties, sensitivity, KS, serialization and "
         "invalid-input checks passed");
    return 0;
}
