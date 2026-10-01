#ifndef TSIM_RNG_H
#define TSIM_RNG_H

#include <stdint.h>

/* A seeded random stream: xoshiro256** (Blackman and Vigna, public domain), seeded through
 * splitmix64.
 *
 * Every source of randomness in a run gets its own stream, derived from the run's seed and a
 * stream number - one per node for its traffic, one for shadowing, and so on - so adding a draw
 * in one place does not shift every number drawn after it somewhere else, and a change to one
 * candidate's MAC does not reshuffle another's traffic. */
struct tsim_rng {
    uint64_t s[4];
};

/* Seeds a stream from the run's seed and a stream number. The same pair always gives the same
 * sequence, on every platform. */
void tsim_rng_init(struct tsim_rng *rng, uint64_t seed, uint64_t stream);

/* The next 64 random bits. */
uint64_t tsim_rng_next(struct tsim_rng *rng);

/* A uniform integer in [0, bound), without modulo bias. A bound of 0 returns 0. */
uint64_t tsim_rng_below(struct tsim_rng *rng, uint64_t bound);

/* A uniform double in [0, 1), with 53 random bits. */
double tsim_rng_unit(struct tsim_rng *rng);

/* A standard normal deviate (Box-Muller). It goes through libm's log, sqrt and cos, so unlike the
 * draws above its last bit can differ between C libraries; runs on one platform still repeat. */
double tsim_rng_normal(struct tsim_rng *rng);

#endif
