#include "tsim/rng.h"

#include "check.h"

/* Known answers from a separate Python implementation of the same seeding and generator. They
 * pin the sequence: a change here changes every run's results, which has to be deliberate. */
static void known_answers(void) {
    struct tsim_rng r;
    tsim_rng_init(&r, 0, 0);
    CHECK_EQ_U64(tsim_rng_next(&r), 0xfb5405f7bd79c540ULL);
    CHECK_EQ_U64(tsim_rng_next(&r), 0x780c98e26cea5883ULL);
    CHECK_EQ_U64(tsim_rng_next(&r), 0x2a146e0980febc66ULL);

    tsim_rng_init(&r, 42, 7);
    CHECK_EQ_U64(tsim_rng_next(&r), 0x24bfb39aeb008c15ULL);
    CHECK_EQ_U64(tsim_rng_next(&r), 0xd858489e7fc02496ULL);
    CHECK_EQ_U64(tsim_rng_next(&r), 0x3deebe24036964f3ULL);
}

static void same_seed_and_stream_repeat(void) {
    struct tsim_rng a, b;
    tsim_rng_init(&a, 1234, 5);
    tsim_rng_init(&b, 1234, 5);
    for (int i = 0; i < 1000; i++) {
        CHECK_EQ_U64(tsim_rng_next(&a), tsim_rng_next(&b));
    }
}

static void streams_differ(void) {
    struct tsim_rng a, b, c;
    tsim_rng_init(&a, 1234, 0);
    tsim_rng_init(&b, 1234, 1);
    tsim_rng_init(&c, 1235, 0);
    uint64_t x = tsim_rng_next(&a);
    CHECK(x != tsim_rng_next(&b));
    CHECK(x != tsim_rng_next(&c));
}

static void below_stays_in_range_and_covers_it(void) {
    struct tsim_rng r;
    tsim_rng_init(&r, 99, 0);
    int seen[10] = {0};
    for (int i = 0; i < 10000; i++) {
        uint64_t x = tsim_rng_below(&r, 10);
        CHECK(x < 10);
        if (x < 10) {
            seen[x]++;
        }
    }
    for (int i = 0; i < 10; i++) {
        /* 1000 expected per bucket; 800 is more than six standard deviations out. */
        CHECK(seen[i] > 800);
    }
    CHECK_EQ_U64(tsim_rng_below(&r, 0), 0);
    CHECK_EQ_U64(tsim_rng_below(&r, 1), 0);
}

static void unit_is_half_open(void) {
    struct tsim_rng r;
    tsim_rng_init(&r, 7, 3);
    double sum = 0;
    for (int i = 0; i < 100000; i++) {
        double u = tsim_rng_unit(&r);
        CHECK(u >= 0.0 && u < 1.0);
        sum += u;
    }
    double mean = sum / 100000;
    CHECK(mean > 0.49 && mean < 0.51);
}

int main(void) {
    RUN(known_answers);
    RUN(same_seed_and_stream_repeat);
    RUN(streams_differ);
    RUN(below_stays_in_range_and_covers_it);
    RUN(unit_is_half_open);
    return CHECK_DONE();
}
