#include "data.h"
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define LINE_SIZE 32768u
#define MAX_COLUMNS 128u
static void errorf(char *e, size_t line, const char *fmt, ...) {
    int n = snprintf(e, RD_ERROR_SIZE, "line %zu: ", line);
    va_list a;
    va_start(a, fmt);
    vsnprintf(e + n, RD_ERROR_SIZE - (size_t)n, fmt, a);
    va_end(a);
}
/* Reject truncated physical lines, NUL, non-ASCII and hidden controls. */
static int read_line(FILE *f, char *s) {
    size_t n = 0;
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n') {
        if (n + 1 >= LINE_SIZE || (c < 32 && c != '\t' && c != '\r') || c > 126)
            return -1;
        s[n++] = (char)c;
    }
    if (ferror(f))
        return -1;
    if (c == EOF && !n)
        return 0;
    if (n && s[n - 1] == '\r')
        --n;
    s[n] = 0;
    if (strchr(s, '\r'))
        return -1;
    return 1;
}
static int csv_split(char *s, char **out, unsigned *count) {
    char *r = s, *w = s;
    unsigned n = 0;
    for (;;) {
        if (n >= MAX_COLUMNS)
            return 0;
        out[n++] = w;
        if (*r == '"') {
            ++r;
            for (;;) {
                if (!*r)
                    return 0;
                if (*r == '"') {
                    ++r;
                    if (*r != '"')
                        break;
                }
                *w++ = *r++;
            }
            if (*r && *r != ',')
                return 0;
        } else {
            while (*r && *r != ',') {
                if (*r == '"')
                    return 0;
                *w++ = *r++;
            }
        }
        int more = *r == ',';
        if (more)
            ++r;
        *w++ = 0;
        if (!more)
            break;
    }
    *count = n;
    return 1;
}
static int tab_split(char *s, char **out, unsigned *count) {
    unsigned n = 0;
    for (;;) {
        if (n >= MAX_COLUMNS)
            return 0;
        out[n++] = s;
        char *next = strchr(s, '\t');
        if (!next)
            break;
        *next = 0;
        s = next + 1;
    }
    *count = n;
    return 1;
}
static int numeric(const char *s, double *out) {
    if (!*s || *s == ' ' || *s == '\t')
        return 0;
    /* Decimal grammar only: reject hexadecimal, NaN and infinities. */
    const char *p = s;
    if (*p == '+' || *p == '-')
        ++p;
    unsigned digits = 0;
    while (*p >= '0' && *p <= '9') {
        ++p;
        ++digits;
    }
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            ++p;
            ++digits;
        }
    }
    if (!digits)
        return 0;
    if (*p == 'e' || *p == 'E') {
        ++p;
        if (*p == '+' || *p == '-')
            ++p;
        unsigned exp_digits = 0;
        while (*p >= '0' && *p <= '9') {
            ++p;
            ++exp_digits;
        }
        if (!exp_digits)
            return 0;
    }
    if (*p)
        return 0;
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (*end || errno || !isfinite(v) || fabs(v) > 1e100)
        return 0;
    *out = v;
    return 1;
}
static int identifier(const char *s, size_t max) {
    if (!s)
        return 0;
    size_t n = strlen(s);
    if (!n || n > max)
        return 0;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126)
            return 0;
    return 1;
}
static int feature_name(const char *s) {
    if (!s || !identifier(s, RD_NAME_SIZE - 1) ||
        !((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_'))
        return 0;
    for (; *s; ++s)
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ||
              *s == '_'))
            return 0;
    return 1;
}
static int append(dataset *d, size_t *capacity, const char *id, const double *row, size_t limit) {
    if (d->rows >= limit || !identifier(id, 255))
        return 0;
    if (d->rows == *capacity) {
        size_t cap = *capacity ? *capacity * 2 : 128;
        if (cap > limit)
            cap = limit;
        double *v = (double *)realloc(d->values, cap * d->features * sizeof(double));
        if (!v)
            return 0;
        d->values = v;
        char **ids = (char **)realloc(d->ids, cap * sizeof(char *));
        if (!ids)
            return 0;
        d->ids = ids;
        *capacity = cap;
    }
    char *copy = (char *)malloc(strlen(id) + 1);
    if (!copy)
        return 0;
    strcpy(copy, id);
    d->ids[d->rows] = copy;
    memcpy(d->values + d->rows * d->features, row, d->features * sizeof(double));
    ++d->rows;
    return 1;
}
void data_free(dataset *d) {
    for (size_t i = 0; i < d->rows; ++i)
        free(d->ids[i]);
    free(d->ids);
    free(d->values);
    memset(d, 0, sizeof(*d));
}
int data_read(const char *path, dataset *d, size_t limit, char *e) {
    memset(d, 0, sizeof(*d));
    *e = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(e, RD_ERROR_SIZE, "cannot open input: %s", path);
        return 0;
    }
    char line[LINE_SIZE], *fields[MAX_COLUMNS] = {0};
    size_t at = 0, capacity = 0;
    unsigned count;
    int status;
    /* Spreadsheet exports often start with a UTF-8 byte order mark; skip it, reject other bytes. */
    unsigned char bom[3];
    size_t got = fread(bom, 1, 3, f);
    if (!(got == 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF))
        rewind(f);
    while ((status = read_line(f, line)) > 0) {
        ++at;
        if (!csv_split(line, fields, &count)) {
            errorf(e, at, "invalid single-line CSV");
            goto bad;
        }
        if (at == 1) {
            if (count < 2 || count > RD_MAX_FEATURES + 1 || strcmp(fields[0], "id")) {
                errorf(e, at, "header must be id plus 1..32 feature names");
                goto bad;
            }
            d->features = count - 1;
            for (unsigned j = 0; j < d->features; ++j) {
                if (!feature_name(fields[j + 1])) {
                    errorf(e, at, "invalid feature name");
                    goto bad;
                }
                for (unsigned k = 0; k < j; ++k)
                    if (!strcmp(fields[j + 1], d->names[k])) {
                        errorf(e, at, "duplicate feature name");
                        goto bad;
                    }
                strcpy(d->names[j], fields[j + 1]);
            }
        } else {
            if (count != d->features + 1) {
                errorf(e, at, "wrong column count");
                goto bad;
            }
            double row[RD_MAX_FEATURES];
            for (unsigned j = 0; j < d->features; ++j)
                if (!numeric(fields[j + 1], &row[j])) {
                    errorf(e, at, "invalid numeric value in %s", d->names[j]);
                    goto bad;
                }
            if (!append(d, &capacity, fields[0], row, limit)) {
                errorf(e, at, "invalid id, row limit exceeded, or out of memory");
                goto bad;
            }
        }
    }
    if (status < 0) {
        errorf(e, at + 1, "invalid byte, oversized line, or read error");
        goto bad;
    }
    if (!d->rows) {
        errorf(e, at, "input has no observations");
        goto bad;
    }
    fclose(f);
    return 1;
bad:
    fclose(f);
    data_free(d);
    return 0;
}
void json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\')
            fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}
static void csv_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; ++s) {
        if (*s == '"')
            fputc('"', f);
        fputc(*s, f);
    }
    fputc('"', f);
}
void data_csv(const dataset *d, FILE *f) {
    fputs("id", f);
    for (unsigned j = 0; j < d->features; ++j)
        fprintf(f, ",%s", d->names[j]);
    fputc('\n', f);
    for (size_t i = 0; i < d->rows; ++i) {
        csv_string(f, d->ids[i]);
        for (unsigned j = 0; j < d->features; ++j)
            fprintf(f, ",%.17g", d->values[i * d->features + j]);
        fputc('\n', f);
    }
}
int zeek_read(const char *path, dataset *d, char *e) {
    static const char *required[] = {"uid",        "proto",        "duration",
                                     "orig_bytes", "resp_bytes",   "orig_pkts",
                                     "resp_pkts",  "missed_bytes", "conn_state"};
    static const char *names[] = {"log_duration",    "log_orig_bytes",
                                  "log_resp_bytes",  "log_orig_pkts",
                                  "log_resp_pkts",   "log_missed_bytes",
                                  "orig_byte_ratio", "orig_packet_ratio",
                                  "log_byte_rate",   "is_tcp",
                                  "is_udp",          "is_failed",
                                  "is_reset",        "missing_measurements"};
    memset(d, 0, sizeof(*d));
    *e = 0;
    d->features = 14;
    for (unsigned j = 0; j < d->features; ++j)
        strcpy(d->names[j], names[j]);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(e, RD_ERROR_SIZE, "cannot open Zeek input");
        return 0;
    }
    char line[LINE_SIZE], *fields[MAX_COLUMNS];
    unsigned count = 0, header_count = 0, mapping[9] = {0};
    int separator = 0, have_header = 0, status;
    size_t at = 0, capacity = 0;
    while ((status = read_line(f, line)) > 0) {
        ++at;
        if (line[0] == '#') {
            if (!strncmp(line, "#separator ", 11)) {
                if (strcmp(line, "#separator \\x09")) {
                    errorf(e, at, "only Zeek TSV separator \\x09 is supported");
                    goto bad;
                }
                separator = 1;
            } else if (!strncmp(line, "#unset_field", 12)) {
                if (strcmp(line, "#unset_field\t-")) {
                    errorf(e, at, "only the default Zeek unset field '-' is supported");
                    goto bad;
                }
            } else if (!strncmp(line, "#path\t", 6)) {
                if (strcmp(line + 6, "conn")) {
                    errorf(e, at, "expected a conn log");
                    goto bad;
                }
            } else if (!strncmp(line, "#fields\t", 8)) {
                if (!separator || !tab_split(line + 8, fields, &count)) {
                    errorf(e, at, "invalid Zeek header");
                    goto bad;
                }
                header_count = count;
                for (unsigned j = 0; j < count; ++j)
                    for (unsigned k = 0; k < j; ++k)
                        if (!strcmp(fields[j], fields[k])) {
                            errorf(e, at, "duplicate Zeek field");
                            goto bad;
                        }
                for (unsigned j = 0; j < 9; ++j) {
                    unsigned k = 0;
                    for (; k < count; ++k)
                        if (!strcmp(required[j], fields[k]))
                            break;
                    if (k == count) {
                        errorf(e, at, "missing Zeek field %s", required[j]);
                        goto bad;
                    }
                    mapping[j] = k;
                }
                have_header = 1;
            }
            continue;
        }
        if (!have_header || !tab_split(line, fields, &count) || count != header_count) {
            errorf(e, at, "missing header or wrong Zeek column count");
            goto bad;
        }
        double x[6], missing = 0;
        for (unsigned j = 0; j < 6; ++j) {
            const char *s = fields[mapping[j + 2]];
            if (!strcmp(s, "-")) {
                x[j] = 0;
                missing += 1;
            } else if (!numeric(s, &x[j]) || x[j] < 0 || (j && floor(x[j]) != x[j])) {
                errorf(e, at, "invalid nonnegative Zeek measurement");
                goto bad;
            }
        }
        const char *proto = fields[mapping[1]], *state = fields[mapping[8]],
                   *id = fields[mapping[0]];
        if (strcmp(proto, "tcp") && strcmp(proto, "udp") && strcmp(proto, "icmp") &&
            strcmp(proto, "unknown")) {
            errorf(e, at, "unsupported Zeek protocol");
            goto bad;
        }
        static const char *states[] = {"S0",   "S1",     "SF",    "REJ", "S2",  "S3", "RSTO",
                                       "RSTR", "RSTOS0", "RSTRH", "SH",  "SHR", "OTH"};
        unsigned valid = 0;
        for (unsigned j = 0; j < 13; ++j)
            if (!strcmp(state, states[j]))
                valid = 1;
        if (!valid || !strcmp(id, "-") || !*id) {
            errorf(e, at, "invalid connection state or uid");
            goto bad;
        }
        double bytes = x[1] + x[2], packets = x[3] + x[4];
        double row[14];
        for (unsigned j = 0; j < 6; ++j)
            row[j] = log1p(x[j]);
        row[6] = bytes > 0 ? x[1] / bytes : 0;
        row[7] = packets > 0 ? x[3] / packets : 0;
        row[8] = log1p(bytes / fmax(x[0], 0.001));
        row[9] = !strcmp(proto, "tcp");
        row[10] = !strcmp(proto, "udp");
        row[11] = !strcmp(state, "S0") || !strcmp(state, "REJ") || !strcmp(state, "RSTOS0") ||
                  !strcmp(state, "SH");
        row[12] = !strncmp(state, "RST", 3);
        row[13] = missing;
        if (!append(d, &capacity, id, row, RD_MAX_ROWS)) {
            errorf(e, at, "invalid uid, row limit exceeded, or out of memory");
            goto bad;
        }
    }
    if (status < 0 || !d->rows) {
        errorf(e, at + 1, "invalid bytes, read error, oversized line, or empty log");
        goto bad;
    }
    fclose(f);
    return 1;
bad:
    fclose(f);
    data_free(d);
    return 0;
}
