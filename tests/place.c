#include "tsim/place.h"

#include "check.h"

static void uniform_stays_inside_and_repeats(void) {
    struct tsim_pos a[300], b[300];
    tsim_place_uniform(a, 300, 4000, 1000, 7);
    tsim_place_uniform(b, 300, 4000, 1000, 7);
    double max_x = 0, max_y = 0;
    for (int i = 0; i < 300; i++) {
        CHECK(a[i].x >= 0 && a[i].x < 4000 && a[i].y >= 0 && a[i].y < 1000);
        CHECK(a[i].x == b[i].x && a[i].y == b[i].y);
        max_x = a[i].x > max_x ? a[i].x : max_x;
        max_y = a[i].y > max_y ? a[i].y : max_y;
    }
    /* Spread over the whole rectangle, not a corner of it. */
    CHECK(max_x > 3800 && max_y > 950);
    tsim_place_uniform(b, 300, 4000, 1000, 8);
    CHECK(a[0].x != b[0].x);
}

/* Fewer nodes on the same map stand where the first of the many stood. */
static void fewer_nodes_are_a_thinned_map(void) {
    struct tsim_pos many[1000], few[200];
    tsim_place_uniform(many, 1000, 18000, 18000, 3);
    tsim_place_uniform(few, 200, 18000, 18000, 3);
    for (int i = 0; i < 200; i++) {
        CHECK(few[i].x == many[i].x && few[i].y == many[i].y);
    }
}

static void grid_fills_rows_of_the_smallest_square(void) {
    struct tsim_pos p[10];
    tsim_place_grid(p, 10, 100);
    /* Ten need a 4 x 4 square: rows of 4. */
    CHECK(p[0].x == 0 && p[0].y == 0);
    CHECK(p[3].x == 300 && p[3].y == 0);
    CHECK(p[4].x == 0 && p[4].y == 100);
    CHECK(p[9].x == 100 && p[9].y == 200);
    tsim_place_grid(p, 9, 50);
    CHECK(p[8].x == 100 && p[8].y == 100);
}

static void line_runs_along_x(void) {
    struct tsim_pos p[4];
    tsim_place_line(p, 4, 2000);
    for (int i = 0; i < 4; i++) {
        CHECK(p[i].x == 2000.0 * i && p[i].y == 0);
    }
}

int main(void) {
    RUN(uniform_stays_inside_and_repeats);
    RUN(fewer_nodes_are_a_thinned_map);
    RUN(grid_fills_rows_of_the_smallest_square);
    RUN(line_runs_along_x);
    return CHECK_DONE();
}
