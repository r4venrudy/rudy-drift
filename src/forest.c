#include "rudy_drift.h"
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(double) == 8 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "Rudy Drift requires IEEE 754 binary64 doubles");

typedef struct {
    uint32_t feature, left, right, count;
    double split;
} node;
typedef struct {
    node *nodes;
    uint32_t used;
} tree;
struct rd_model {
    unsigned features, trees, sample;
    size_t calibration_rows;
    uint64_t seed;
    char names[RD_MAX_FEATURES][RD_NAME_SIZE];
    double medians[RD_MAX_FEATURES];
    double *scores, *reference, *path_correction;
    tree *forest;
};

static void fail(char *e, const char *fmt, ...) {
    if (e) {
        va_list a;
        va_start(a, fmt);
        vsnprintf(e, RD_ERROR_SIZE, fmt, a);
        va_end(a);
    }
}
static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static int finite_value(double x) { return isfinite(x) && fabs(x) <= 1e100; }
static int name_ok(const char *s) {
    size_t n = 0;
    if (!s || !((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_'))
        return 0;
    for (; s[n]; ++n) {
        unsigned char c = (unsigned char)s[n];
        if (n >= RD_NAME_SIZE - 1 || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                       (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return n > 0;
}
/* SplitMix64: fixed sequence across platforms; not a cryptographic generator. */
static uint64_t random64(uint64_t *s) {
    uint64_t z = (*s += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}
static uint64_t bounded(uint64_t *s, uint64_t n) {
    uint64_t threshold = (UINT64_C(0) - n) % n, r;
    do {
        r = random64(s);
    } while (r < threshold);
    return r % n;
}
static rd_model *allocate(unsigned f, unsigned t, unsigned sample, size_t cal) {
    rd_model *m = (rd_model *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->features = f;
    m->trees = t;
    m->sample = sample;
    m->calibration_rows = cal;
    m->forest = (tree *)calloc(t, sizeof(tree));
    m->scores = (double *)calloc(cal, sizeof(double));
    m->reference = (double *)calloc(cal * f, sizeof(double));
    m->path_correction = (double *)calloc(sample + 1u, sizeof(double));
    if (!m->forest || !m->scores || !m->reference || !m->path_correction) {
        rd_free(m);
        return NULL;
    }
    double harmonic = 0;
    for (unsigned i = 2; i <= sample; ++i) {
        harmonic += 1.0 / (double)(i - 1u);
        m->path_correction[i] = 2.0 * harmonic - 2.0 * (double)(i - 1u) / (double)i;
    }
    return m;
}
void rd_free(rd_model *m) {
    if (!m)
        return;
    if (m->forest)
        for (unsigned i = 0; i < m->trees; ++i)
            free(m->forest[i].nodes);
    free(m->forest);
    free(m->scores);
    free(m->reference);
    free(m->path_correction);
    free(m);
}
unsigned rd_features(const rd_model *m) { return m ? m->features : 0; }
const char *rd_feature_name(const rd_model *m, unsigned f) {
    return m && f < m->features ? m->names[f] : NULL;
}
double rd_median(const rd_model *m, unsigned f) {
    return m && f < m->features ? m->medians[f] : NAN;
}
size_t rd_calibration_rows(const rd_model *m) { return m ? m->calibration_rows : 0; }

static uint32_t build(tree *t, const double *x, size_t *indices, unsigned count, unsigned features,
                      unsigned depth, unsigned limit, uint64_t *rng) {
    uint32_t at = t->used++;
    node *n = &t->nodes[at];
    n->feature = UINT32_MAX;
    n->count = count;
    if (count < 2 || depth >= limit)
        return at;
    unsigned candidates[RD_MAX_FEATURES], available = 0;
    double minima[RD_MAX_FEATURES], maxima[RD_MAX_FEATURES];
    for (unsigned f = 0; f < features; ++f) {
        double lo = x[indices[0] * features + f], hi = lo;
        for (unsigned i = 1; i < count; ++i) {
            double v = x[indices[i] * features + f];
            if (v < lo)
                lo = v;
            if (v > hi)
                hi = v;
        }
        minima[f] = lo;
        maxima[f] = hi;
        if (lo < hi)
            candidates[available++] = f;
    }
    if (!available)
        return at;
    unsigned f = candidates[bounded(rng, available)];
    double u = (double)(random64(rng) >> 11) * 0x1.0p-53;
    double split = minima[f] + u * (maxima[f] - minima[f]);
    /* Rounding may produce max. Using <= and split<max guarantees two children. */
    if (split >= maxima[f])
        split = nextafter(maxima[f], minima[f]);
    if (split < minima[f])
        split = minima[f];
    unsigned left = 0;
    for (unsigned i = 0; i < count; ++i)
        if (x[indices[i] * features + f] <= split) {
            size_t tmp = indices[left];
            indices[left++] = indices[i];
            indices[i] = tmp;
        }
    n->feature = f;
    n->split = split;
    n->left = build(t, x, indices, left, features, depth + 1u, limit, rng);
    n->right = build(t, x, indices + left, count - left, features, depth + 1u, limit, rng);
    return at;
}
static double score(const rd_model *m, const double *row) {
    double paths = 0;
    for (unsigned i = 0; i < m->trees; ++i) {
        const tree *t = &m->forest[i];
        unsigned depth = 0;
        const node *n = &t->nodes[0];
        while (n->feature != UINT32_MAX) {
            n = &t->nodes[row[n->feature] <= n->split ? n->left : n->right];
            ++depth;
        }
        paths += (double)depth + m->path_correction[n->count];
    }
    return exp2(-paths / ((double)m->trees * m->path_correction[m->sample]));
}
int rd_predict(const rd_model *m, const double *row, rd_prediction *out) {
    if (!m || !row || !out)
        return 0;
    for (unsigned f = 0; f < m->features; ++f)
        if (!finite_value(row[f]))
            return 0;
    double s = score(m, row);
    size_t lo = 0, hi = m->calibration_rows;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (m->scores[mid] < s)
            lo = mid + 1;
        else
            hi = mid;
    }
    out->score = s;
    out->p_value = (double)(1 + m->calibration_rows - lo) / (double)(1 + m->calibration_rows);
    return 1;
}
int rd_explain(const rd_model *m, const double *row, double *deltas) {
    rd_prediction p;
    if (!deltas || !rd_predict(m, row, &p))
        return 0;
    double copy[RD_MAX_FEATURES];
    memcpy(copy, row, m->features * sizeof(double));
    for (unsigned f = 0; f < m->features; ++f) {
        copy[f] = m->medians[f];
        deltas[f] = p.score - score(m, copy);
        copy[f] = row[f];
    }
    return 1;
}
rd_model *rd_fit(const double *x, size_t rows, const double *cal, size_t cal_rows, unsigned f,
                 const char *const *names, rd_config cfg, char *error) {
    if (error)
        *error = 0;
    if (!x || !cal || !names || !f || f > RD_MAX_FEATURES || rows < 2 || rows > RD_MAX_ROWS ||
        cal_rows < 20 || cal_rows > RD_MAX_CALIBRATION || cfg.trees < 1 || cfg.trees > 512 ||
        cfg.sample_size < 2 || cfg.sample_size > 4096 || cfg.sample_size > rows) {
        fail(error, "invalid fit dimensions/configuration (calibration requires 20..8192 rows)");
        return NULL;
    }
    for (unsigned j = 0; j < f; ++j) {
        if (!name_ok(names[j])) {
            fail(error, "invalid feature name");
            return NULL;
        }
        for (unsigned k = 0; k < j; ++k)
            if (!strcmp(names[k], names[j])) {
                fail(error, "duplicate feature name");
                return NULL;
            }
    }
    for (size_t i = 0; i < rows * f; ++i)
        if (!finite_value(x[i])) {
            fail(error, "nonfinite or out-of-range training value");
            return NULL;
        }
    for (size_t i = 0; i < cal_rows * f; ++i)
        if (!finite_value(cal[i])) {
            fail(error, "nonfinite or out-of-range calibration value");
            return NULL;
        }
    rd_model *m = allocate(f, cfg.trees, cfg.sample_size, cal_rows);
    size_t *indices = (size_t *)malloc(rows * sizeof(size_t));
    double *scratch = (double *)malloc(rows * sizeof(double));
    if (!m || !indices || !scratch) {
        fail(error, "out of memory");
        rd_free(m);
        free(indices);
        free(scratch);
        return NULL;
    }
    m->seed = cfg.seed;
    for (unsigned j = 0; j < f; ++j) {
        strcpy(m->names[j], names[j]);
        for (size_t i = 0; i < rows; ++i)
            scratch[i] = x[i * f + j];
        qsort(scratch, rows, sizeof(double), cmp_double);
        m->medians[j] =
            rows % 2 ? scratch[rows / 2] : scratch[rows / 2 - 1] / 2 + scratch[rows / 2] / 2;
        for (size_t i = 0; i < cal_rows; ++i)
            m->reference[j * cal_rows + i] = cal[i * f + j];
        qsort(m->reference + j * cal_rows, cal_rows, sizeof(double), cmp_double);
    }
    unsigned limit = 0, power = 1;
    while (power < cfg.sample_size) {
        power *= 2;
        ++limit;
    }
    uint64_t rng = cfg.seed;
    for (unsigned t = 0; t < cfg.trees; ++t) {
        tree *tr = &m->forest[t];
        tr->nodes = (node *)calloc(2u * cfg.sample_size - 1u, sizeof(node));
        if (!tr->nodes) {
            fail(error, "out of memory");
            rd_free(m);
            free(indices);
            free(scratch);
            return NULL;
        }
        for (size_t i = 0; i < rows; ++i)
            indices[i] = i;
        for (unsigned i = 0; i < cfg.sample_size; ++i) {
            size_t j = i + (size_t)bounded(&rng, rows - i), tmp = indices[i];
            indices[i] = indices[j];
            indices[j] = tmp;
        }
        build(tr, x, indices, cfg.sample_size, f, 0, limit, &rng);
    }
    for (size_t i = 0; i < cal_rows; ++i)
        m->scores[i] = score(m, cal + i * f);
    qsort(m->scores, cal_rows, sizeof(double), cmp_double);
    free(indices);
    free(scratch);
    return m;
}
int rd_ks(const rd_model *m, unsigned f, const double *values, size_t rows, double *distance) {
    if (!m || f >= m->features || !values || !distance || !rows || rows > RD_MAX_ROWS)
        return 0;
    double *b = (double *)malloc(rows * sizeof(double));
    if (!b)
        return 0;
    for (size_t i = 0; i < rows; ++i) {
        if (!finite_value(values[i])) {
            free(b);
            return 0;
        }
        b[i] = values[i];
    }
    qsort(b, rows, sizeof(double), cmp_double);
    const double *a = m->reference + f * m->calibration_rows;
    size_t i = 0, j = 0;
    double d = 0;
    while (i < m->calibration_rows || j < rows) {
        double v = j == rows || (i < m->calibration_rows && a[i] < b[j]) ? a[i] : b[j];
        while (i < m->calibration_rows && a[i] <= v)
            ++i;
        while (j < rows && b[j] <= v)
            ++j;
        double delta = fabs((double)i / (double)m->calibration_rows - (double)j / (double)rows);
        if (delta > d)
            d = delta;
    }
    free(b);
    *distance = d;
    return 1;
}

/* Streaming, portable encoding. No pointers or native struct padding on disk. */
typedef struct {
    FILE *f;
    uint32_t crc;
    int ok;
} codec;
static uint32_t crc_byte(uint32_t crc, unsigned char b) {
    crc ^= b;
    for (int i = 0; i < 8; ++i)
        crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1u));
    return crc;
}
static void put(codec *c, uint64_t v, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        unsigned char b = (unsigned char)(v >> (8u * i));
        if (fputc(b, c->f) == EOF)
            c->ok = 0;
        c->crc = crc_byte(c->crc, b);
    }
}
static uint64_t get(codec *c, unsigned bytes) {
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; ++i) {
        int r = fgetc(c->f);
        if (r == EOF) {
            c->ok = 0;
            return 0;
        }
        c->crc = crc_byte(c->crc, (unsigned char)r);
        v |= (uint64_t)(unsigned char)r << (8u * i);
    }
    return v;
}
static void put_double(codec *c, double d) {
    uint64_t v;
    memcpy(&v, &d, 8);
    put(c, v, 8);
}
static double get_double(codec *c) {
    uint64_t v = get(c, 8);
    double d;
    memcpy(&d, &v, 8);
    return d;
}
int rd_save(const rd_model *m, FILE *f, char *e) {
    if (e)
        *e = 0;
    if (!m || !f) {
        fail(e, "null model or stream");
        return 0;
    }
    codec c = {f, UINT32_MAX, 1};
    const char magic[8] = {'R', 'D', 'R', 'I', 'F', 'T', 0, 1};
    for (unsigned i = 0; i < 8; ++i)
        put(&c, (unsigned char)magic[i], 1);
    put(&c, m->features, 4);
    put(&c, m->trees, 4);
    put(&c, m->sample, 4);
    put(&c, m->calibration_rows, 4);
    put(&c, m->seed, 8);
    for (unsigned j = 0; j < m->features; ++j) {
        for (unsigned k = 0; k < RD_NAME_SIZE; ++k)
            put(&c, (unsigned char)m->names[j][k], 1);
        put_double(&c, m->medians[j]);
    }
    for (size_t i = 0; i < m->calibration_rows; ++i)
        put_double(&c, m->scores[i]);
    for (size_t i = 0; i < m->calibration_rows * m->features; ++i)
        put_double(&c, m->reference[i]);
    for (unsigned t = 0; t < m->trees; ++t) {
        put(&c, m->forest[t].used, 4);
        for (uint32_t i = 0; i < m->forest[t].used; ++i) {
            const node *n = &m->forest[t].nodes[i];
            put(&c, n->feature, 4);
            put(&c, n->left, 4);
            put(&c, n->right, 4);
            put(&c, n->count, 4);
            put_double(&c, n->split);
        }
    }
    uint32_t checksum = ~c.crc;
    put(&c, checksum, 4);
    if (!c.ok || fflush(f)) {
        fail(e, "cannot write model");
        return 0;
    }
    return 1;
}
rd_model *rd_load(FILE *f, char *e) {
    if (e)
        *e = 0;
    if (!f) {
        fail(e, "null stream");
        return NULL;
    }
    codec c = {f, UINT32_MAX, 1};
    const unsigned char magic[8] = {'R', 'D', 'R', 'I', 'F', 'T', 0, 1};
    for (unsigned i = 0; i < 8; ++i)
        if (get(&c, 1) != magic[i]) {
            fail(e, "invalid model magic/version");
            return NULL;
        }
    unsigned features = (unsigned)get(&c, 4), trees = (unsigned)get(&c, 4),
             sample = (unsigned)get(&c, 4);
    size_t cal = (size_t)get(&c, 4);
    uint64_t seed = get(&c, 8);
    if (!c.ok || !features || features > RD_MAX_FEATURES || !trees || trees > 512 || sample < 2 ||
        sample > 4096 || cal < 20 || cal > RD_MAX_CALIBRATION) {
        fail(e, "invalid model dimensions");
        return NULL;
    }
    rd_model *m = allocate(features, trees, sample, cal);
    if (!m) {
        fail(e, "out of memory");
        return NULL;
    }
    m->seed = seed;
    for (unsigned j = 0; j < features; ++j) {
        for (unsigned k = 0; k < RD_NAME_SIZE; ++k)
            m->names[j][k] = (char)get(&c, 1);
        if (m->names[j][RD_NAME_SIZE - 1] != 0 || !name_ok(m->names[j]))
            goto invalid;
        for (unsigned k = 0; k < j; ++k)
            if (!strcmp(m->names[j], m->names[k]))
                goto invalid;
        m->medians[j] = get_double(&c);
        if (!finite_value(m->medians[j]))
            goto invalid;
    }
    for (size_t i = 0; i < cal; ++i) {
        m->scores[i] = get_double(&c);
        if (!isfinite(m->scores[i]) || m->scores[i] <= 0 || m->scores[i] > 1 ||
            (i && m->scores[i] < m->scores[i - 1]))
            goto invalid;
    }
    for (unsigned j = 0; j < features; ++j)
        for (size_t i = 0; i < cal; ++i) {
            size_t at = j * cal + i;
            m->reference[at] = get_double(&c);
            if (!finite_value(m->reference[at]) || (i && m->reference[at] < m->reference[at - 1]))
                goto invalid;
        }
    unsigned max_depth = 0, power = 1;
    while (power < sample) {
        power *= 2;
        ++max_depth;
    }
    for (unsigned t = 0; t < trees; ++t) {
        tree *tr = &m->forest[t];
        tr->used = (uint32_t)get(&c, 4);
        if (!c.ok || !tr->used || tr->used > 2u * sample - 1u)
            goto invalid;
        tr->nodes = (node *)calloc(tr->used, sizeof(node));
        if (!tr->nodes)
            goto invalid;
        unsigned char incoming[8191] = {0}, depth[8191] = {0};
        for (uint32_t i = 0; i < tr->used; ++i) {
            node *n = &tr->nodes[i];
            n->feature = (uint32_t)get(&c, 4);
            n->left = (uint32_t)get(&c, 4);
            n->right = (uint32_t)get(&c, 4);
            n->count = (uint32_t)get(&c, 4);
            n->split = get_double(&c);
            if (!c.ok || !n->count || n->count > sample || !finite_value(n->split) ||
                (i && incoming[i] != 1))
                goto invalid;
            if (n->feature == UINT32_MAX) {
                if (n->left || n->right || n->split != 0)
                    goto invalid;
            } else {
                if (n->feature >= features || n->left <= i || n->right <= i ||
                    n->left >= tr->used || n->right >= tr->used || n->left == n->right ||
                    depth[i] >= max_depth || n->count < 2)
                    goto invalid;
                if (++incoming[n->left] != 1 || ++incoming[n->right] != 1)
                    goto invalid;
                depth[n->left] = (unsigned char)(depth[i] + 1u);
                depth[n->right] = (unsigned char)(depth[i] + 1u);
            }
        }
        if (tr->nodes[0].count != sample)
            goto invalid;
        for (uint32_t i = 0; i < tr->used; ++i) {
            const node *n = &tr->nodes[i];
            if (n->feature != UINT32_MAX &&
                tr->nodes[n->left].count + tr->nodes[n->right].count != n->count)
                goto invalid;
        }
    }
    {
        uint32_t expected = ~c.crc, actual = (uint32_t)get(&c, 4);
        if (!c.ok || actual != expected || fgetc(f) != EOF || ferror(f))
            goto invalid;
    }
    return m;
invalid:
    fail(e, "invalid, truncated, or corrupted model");
    rd_free(m);
    return NULL;
}
