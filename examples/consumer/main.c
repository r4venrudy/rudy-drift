#include <errno.h>
#include <math.h>
#include <rudy_drift.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 3) {
        fputs("usage: drift-consumer model.rdm value [value...]\n", stderr);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 1;
    char error[RD_ERROR_SIZE];
    rd_model *m = rd_load(f, error);
    fclose(f);
    if (!m) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    if ((unsigned)(argc - 2) != rd_features(m)) {
        rd_free(m);
        return 2;
    }
    double row[RD_MAX_FEATURES];
    for (unsigned i = 0; i < rd_features(m); ++i) {
        char *end;
        errno = 0;
        row[i] = strtod(argv[i + 2], &end);
        if (errno || *end || end == argv[i + 2] || !isfinite(row[i])) {
            rd_free(m);
            return 2;
        }
    }
    rd_prediction p;
    if (!rd_predict(m, row, &p)) {
        rd_free(m);
        return 1;
    }
    printf("score=%.9g p_value=%.9g\n", p.score, p.p_value);
    rd_free(m);
    return 0;
}
