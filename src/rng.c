#include "tsim/rng.h"

#include <math.h>

static uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

void tsim_rng_init(struct tsim_rng *rng, uint64_t seed, uint64_t stream) {
    /* Mix the stream number in through its own splitmix step, so streams n and n+1 of one seed
     * do not start from neighbouring splitmix states. */
    uint64_t x = seed;
    uint64_t salt = stream;
    x ^= splitmix64(&salt);
    for (int i = 0; i < 4; i++) {
        rng->s[i] = splitmix64(&x);
    }
}

uint64_t tsim_rng_next(struct tsim_rng *rng) {
    uint64_t *s = rng->s;
    uint64_t result = rotl(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
}

uint64_t tsim_rng_below(struct tsim_rng *rng, uint64_t bound) {
    if (bound == 0) {
        return 0;
    }
    /* Reject draws from the incomplete last copy of [0, bound) at the top of the range. */
    uint64_t limit = UINT64_MAX - UINT64_MAX % bound;
    uint64_t x;
    do {
        x = tsim_rng_next(rng);
    } while (x >= limit);
    return x % bound;
}

double tsim_rng_unit(struct tsim_rng *rng) {
    return (double)(tsim_rng_next(rng) >> 11) * 0x1.0p-53;
}

double tsim_rng_normal(struct tsim_rng *rng) {
    /* Box-Muller, keeping only the cosine half so a draw always consumes exactly two numbers. The
     * first is moved from [0, 1) to (0, 1] so the logarithm is finite. */
    double u1 = 1.0 - tsim_rng_unit(rng);
    double u2 = tsim_rng_unit(rng);
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}
