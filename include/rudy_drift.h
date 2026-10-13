#ifndef RUDY_DRIFT_H
#define RUDY_DRIFT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RD_VERSION "0.1.0"
#define RD_MAX_FEATURES 32u
#define RD_NAME_SIZE 49u
#define RD_ERROR_SIZE 256u
#define RD_MAX_ROWS 100000u
#define RD_MAX_CALIBRATION 8192u

typedef struct rd_model rd_model;
typedef struct {
    unsigned trees, sample_size;
    uint64_t seed;
} rd_config;
typedef struct {
    double score, p_value;
} rd_prediction;

/* Matrices are row-major. Training and calibration must be independent,
 * trusted-normal samples. Names must be unique ASCII identifiers (1..48).
 * On failure functions return NULL/0 and optionally fill error[RD_ERROR_SIZE]. */
rd_model *rd_fit(const double *training, size_t rows, const double *calibration,
                 size_t calibration_rows, unsigned features, const char *const *names,
                 rd_config config, char *error);
void rd_free(rd_model *model);
unsigned rd_features(const rd_model *model);
const char *rd_feature_name(const rd_model *model, unsigned feature);
double rd_median(const rd_model *model, unsigned feature);
size_t rd_calibration_rows(const rd_model *model);
int rd_predict(const rd_model *model, const double *row, rd_prediction *out);
/* Score(row) minus score(row with one feature replaced by its training median).
 * Sensitivity only: not a causal explanation or an additive decomposition. */
int rd_explain(const rd_model *model, const double *row, double *deltas);
/* Exact two-sample KS distance, including ties. Values need not be sorted.
 * This reports distribution distance, not a significance test. */
int rd_ks(const rd_model *model, unsigned feature, const double *values, size_t rows,
          double *distance);
/* Binary v1: IEEE binary64, little-endian, bounded tree validation, CRC32.
 * CRC detects corruption, not malicious modification. Caller owns FILEs. */
int rd_save(const rd_model *model, FILE *stream, char *error);
rd_model *rd_load(FILE *stream, char *error);

#ifdef __cplusplus
}
#endif
#endif
