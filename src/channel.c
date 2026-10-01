#include "tsim/channel.h"

#include <math.h>

#include "tsim/rng.h"

/* The shared field: independent unit normals at the corners of a square grid, with a point's
 * value the bilinear blend of its cell's four corners, rescaled so it is a unit normal too.
 * Nothing is stored; a corner's value is drawn from a stream keyed by its coordinates. */

enum { FIELD_DOMAIN = 1, LINK_DOMAIN = 2 };

struct blend {
    uint32_t ix[4]; /* grid cells, modulo 2^32 */
    uint32_t iy[4];
    double w[4]; /* scaled so the squares sum to 1 */
};

/* A unit normal that depends only on (seed, domain, key). The domain is mixed in through a
 * stream of its own, so a grid corner and a link can never draw the same number. */
static double keyed_normal(uint64_t seed, uint64_t domain, uint64_t key) {
    struct tsim_rng r;
    tsim_rng_init(&r, seed, domain);
    tsim_rng_init(&r, tsim_rng_next(&r), key);
    return tsim_rng_normal(&r);
}

/* A grid cell's index modulo 2^32, which is all the field's key keeps of it. fmod is exact, so a
 * coordinate too large for any integer type still names a cell - the same one, for every
 * coordinate an int64_t could hold, as casting and truncating did. */
static uint32_t cell(double floored) {
    double m = fmod(floored, 4294967296.0);
    if (m != m) {
        return 0; /* an infinite coordinate */
    }
    return (uint32_t)(m < 0 ? m + 4294967296.0 : m);
}

static struct blend blend_at(const struct tsim_channel_params *p, struct tsim_pos at) {
    double gx = at.x / p->decorrelation_m;
    double gy = at.y / p->decorrelation_m;
    double fx = floor(gx);
    double fy = floor(gy);
    double tx = gx - fx;
    double ty = gy - fy;
    uint32_t x0 = cell(fx);
    uint32_t y0 = cell(fy);
    struct blend b = {
        .ix = {x0, x0 + 1, x0, x0 + 1},
        .iy = {y0, y0, y0 + 1, y0 + 1},
        .w = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty},
    };
    double norm = 0;
    for (int i = 0; i < 4; i++) {
        norm += b.w[i] * b.w[i];
    }
    norm = sqrt(norm);
    for (int i = 0; i < 4; i++) {
        b.w[i] /= norm;
    }
    return b;
}

static double field_value(const struct tsim_channel_params *p, const struct blend *b) {
    double v = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t key = ((uint64_t)b->ix[i] << 32) | b->iy[i];
        v += b->w[i] * keyed_normal(p->seed, FIELD_DOMAIN, key);
    }
    return v;
}

/* The correlation between the field at two points: the overlap of their corner weights. */
static double field_correlation(const struct blend *a, const struct blend *b) {
    double rho = 0;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (a->ix[i] == b->ix[j] && a->iy[i] == b->iy[j]) {
                rho += a->w[i] * b->w[j];
            }
        }
    }
    return rho;
}

struct tsim_channel_params tsim_channel_default(uint64_t seed) {
    return (struct tsim_channel_params){
        .model = TSIM_PATH_LOG_DISTANCE,
        .pl0_db = 128.95,
        .d0_m = 1000.0,
        .exponent = 2.32,
        .freq_mhz = 915.0,
        .height_m = 1.0,
        .sigma_db = 7.8,
        .node_share = 0.5,
        .decorrelation_m = 100.0,
        .seed = seed,
    };
}

double tsim_channel_median_loss(const struct tsim_channel_params *p, double distance_m) {
    if (distance_m < 1.0) {
        distance_m = 1.0;
    }
    if (p->model == TSIM_PATH_LOG_DISTANCE) {
        return p->pl0_db + 10.0 * p->exponent * log10(distance_m / p->d0_m);
    }
    double h = p->height_m;
    double urban = p->model == TSIM_PATH_3GPP_URBAN ? 3.0 : 0.0;
    return (44.9 - 6.55 * log10(h)) * (log10(distance_m) - 3.0) + 45.5 +
           (35.46 - 1.1 * h) * log10(p->freq_mhz) - 13.82 * log10(h) + 0.7 * h + urban;
}

double tsim_channel_shadowing(const struct tsim_channel_params *p, uint32_t a, struct tsim_pos pa,
                              uint32_t b, struct tsim_pos pb) {
    uint32_t lo = a < b ? a : b;
    uint32_t hi = a < b ? b : a;
    double own = keyed_normal(p->seed, LINK_DOMAIN, ((uint64_t)lo << 32) | hi);

    struct blend ba = blend_at(p, pa);
    struct blend bb = blend_at(p, pb);
    /* The sum of the two endpoints' values has variance 2 + 2 rho, where rho is how correlated
     * the field is between them; dividing by its square root keeps a short link's shadowing as
     * wide as a long one's. */
    double shared =
        (field_value(p, &ba) + field_value(p, &bb)) / sqrt(2.0 + 2.0 * field_correlation(&ba, &bb));

    return p->sigma_db * (sqrt(p->node_share) * shared + sqrt(1.0 - p->node_share) * own);
}

double tsim_channel_loss(const struct tsim_channel_params *p, uint32_t a, struct tsim_pos pa,
                         uint32_t b, struct tsim_pos pb) {
    double distance = hypot(pa.x - pb.x, pa.y - pb.y);
    return tsim_channel_median_loss(p, distance) + tsim_channel_shadowing(p, a, pa, b, pb);
}
