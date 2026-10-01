#include "tsim/channel.h"

#include <math.h>

#include "check.h"

#define CHECK_NEAR(actual, expected, tol) CHECK(fabs((actual) - (expected)) <= (tol))

static void median_loss_follows_log_distance(void) {
    struct tsim_channel_params p = tsim_channel_default(1);
    CHECK_NEAR(tsim_channel_median_loss(&p, 1000.0), 128.95, 1e-9);
    CHECK_NEAR(tsim_channel_median_loss(&p, 10000.0), 128.95 + 23.2, 1e-9);
    CHECK_NEAR(tsim_channel_median_loss(&p, 100.0), 128.95 - 23.2, 1e-9);
    /* Closer than a metre is a metre. */
    CHECK_NEAR(tsim_channel_median_loss(&p, 0.0), tsim_channel_median_loss(&p, 1.0), 1e-9);
}

static void shadowing_is_symmetric_and_repeatable(void) {
    struct tsim_channel_params p = tsim_channel_default(7);
    struct tsim_pos a = {12.0, -40.0};
    struct tsim_pos b = {830.0, 260.0};
    double ab = tsim_channel_shadowing(&p, 3, a, 9, b);
    CHECK(ab == tsim_channel_shadowing(&p, 9, b, 3, a));
    CHECK(ab == tsim_channel_shadowing(&p, 3, a, 9, b));
    struct tsim_channel_params q = tsim_channel_default(8);
    CHECK(ab != tsim_channel_shadowing(&q, 3, a, 9, b));
    CHECK_NEAR(tsim_channel_loss(&p, 3, a, 9, b),
               tsim_channel_median_loss(&p, hypot(818.0, 300.0)) + ab, 1e-9);
}

struct moments {
    double mean;
    double sd;
};

static struct moments moments_of(const double *x, int n) {
    double sum = 0;
    double sq = 0;
    for (int i = 0; i < n; i++) {
        sum += x[i];
    }
    double mean = sum / n;
    for (int i = 0; i < n; i++) {
        sq += (x[i] - mean) * (x[i] - mean);
    }
    return (struct moments){mean, sqrt(sq / (n - 1))};
}

static double correlation(const double *x, const double *y, int n) {
    struct moments mx = moments_of(x, n);
    struct moments my = moments_of(y, n);
    double c = 0;
    for (int i = 0; i < n; i++) {
        c += (x[i] - mx.mean) * (y[i] - my.mean);
    }
    return c / ((n - 1) * mx.sd * my.sd);
}

/* Over many seeds, a link's shadowing is N(0, sigma) - for a long link and for a short one, whose
 * two endpoints read nearly the same value off the shared field. */
static void shadowing_has_the_configured_spread(void) {
    enum { N = 4000 };
    static double far[N];
    static double near[N];
    for (int s = 0; s < N; s++) {
        struct tsim_channel_params p = tsim_channel_default((uint64_t)s);
        far[s] = tsim_channel_shadowing(&p, 0, (struct tsim_pos){0, 0}, 1,
                                        (struct tsim_pos){5000, 3000});
        near[s] =
            tsim_channel_shadowing(&p, 0, (struct tsim_pos){50, 50}, 1, (struct tsim_pos){55, 50});
    }
    struct moments f = moments_of(far, N);
    struct moments n = moments_of(near, N);
    CHECK(fabs(f.mean) < 0.4);
    CHECK(fabs(n.mean) < 0.4);
    CHECK(fabs(f.sd - 7.8) < 0.3);
    CHECK(fabs(n.sd - 7.8) < 0.3);
}

/* Two links whose ends stand a metre apart share their shared half; two links a kilometre apart
 * share nothing. */
static void nearby_links_are_correlated(void) {
    enum { N = 4000 };
    static double x[N];
    static double close[N];
    static double distant[N];
    struct tsim_pos a = {120, 80};
    struct tsim_pos b = {900, 450};
    for (int s = 0; s < N; s++) {
        struct tsim_channel_params p = tsim_channel_default((uint64_t)s);
        x[s] = tsim_channel_shadowing(&p, 0, a, 1, b);
        close[s] = tsim_channel_shadowing(&p, 2, (struct tsim_pos){a.x + 1, a.y}, 3,
                                          (struct tsim_pos){b.x + 1, b.y});
        distant[s] = tsim_channel_shadowing(&p, 4, (struct tsim_pos){a.x + 1000, a.y}, 5,
                                            (struct tsim_pos){b.x + 1000, b.y});
    }
    double rc = correlation(x, close, N);
    double rd = correlation(x, distant, N);
    CHECK(rc > 0.4 && rc < 0.55); /* node_share 0.5, times a field correlation just under 1 */
    CHECK(fabs(rd) < 0.06);
}

/* A coordinate too large for any integer type still names a grid cell: the shadowing comes out
 * finite and repeatable, rather than from a cast the language leaves undefined. */
static void shadowing_holds_at_any_finite_position(void) {
    struct tsim_channel_params p = tsim_channel_default(5);
    struct tsim_pos far = {1e300, -1e300};
    struct tsim_pos near = {1e300, -1e300 + 1e290};
    double x = tsim_channel_shadowing(&p, 0, far, 1, near);
    CHECK(isfinite(x));
    CHECK(x == tsim_channel_shadowing(&p, 0, far, 1, near));
    struct tsim_pos edge = {-9.3e20, 9.3e20}; /* just past what an int64_t cell index holds */
    CHECK(isfinite(tsim_channel_shadowing(&p, 0, edge, 1, far)));
}

int main(void) {
    RUN(median_loss_follows_log_distance);
    RUN(shadowing_is_symmetric_and_repeatable);
    RUN(shadowing_has_the_configured_spread);
    RUN(nearby_links_are_correlated);
    RUN(shadowing_holds_at_any_finite_position);
    return CHECK_DONE();
}
