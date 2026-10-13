#ifndef RD_DATA_H
#define RD_DATA_H
#include "rudy_drift.h"
typedef struct {
    size_t rows;
    unsigned features;
    char names[RD_MAX_FEATURES][RD_NAME_SIZE];
    double *values;
    char **ids;
} dataset;
int data_read(const char *path, dataset *out, size_t limit, char *error);
int zeek_read(const char *path, dataset *out, char *error);
void data_free(dataset *data);
void data_csv(const dataset *data, FILE *out);
void json_string(FILE *out, const char *s);
#endif
