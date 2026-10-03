#ifndef TSIM_PLACE_H
#define TSIM_PLACE_H

#include <stdbool.h>
#include <stdint.h>

#include "tsim/channel.h"
#include "tsim/rng.h"
#include "tsim/time.h"

/* Where the nodes stand. */

/* Uniformly at random over a width x height rectangle with a corner at the origin. Each node's
 * position is drawn from a stream of its own, so the first n nodes stand in the same places
 * whatever the total: a 200-node run is a thinned 1000-node run, not a different map. */
void tsim_place_uniform(struct tsim_pos *pos, uint32_t nodes, double width_m, double height_m,
                        uint64_t seed);

/* On a square grid `spacing_m` apart, row by row, as many columns as the smallest square that
 * holds them all. */
void tsim_place_grid(struct tsim_pos *pos, uint32_t nodes, double spacing_m);

/* In a row along x, `spacing_m` apart. */
void tsim_place_line(struct tsim_pos *pos, uint32_t nodes, double spacing_m);

/* How nodes move (MSH-59): random waypoint. A mover heads in a straight line for a point drawn
 * uniformly over its box, at a speed drawn uniformly from speed_min to speed_max; arrived, it
 * stays for a time drawn from an exponential of mean `pause`, then sets off for the next. It sets
 * off for the first as soon as it starts.
 *
 * speed_min is above 0: with speeds down to 0, the mean speed of a set of movers sinks the longer
 * they move, as the slowest take ever longer to arrive (Yoon, Liu and Noble, "Random waypoint
 * considered harmful", INFOCOM 2003). */
struct tsim_move_params {
    double x_min, y_min, x_max, y_max; /* the box, in metres */
    double speed_min;                  /* m/s */
    double speed_max;
    tsim_time pause; /* mean; 0 sets off again after 1 ms */
};

struct tsim_mover {
    struct tsim_pos at;
    struct tsim_pos to;
    double speed;   /* m/s on the way there */
    tsim_time rest; /* left of the stay where it is, once arrived */
    struct tsim_rng rng;
};

/* Starts a mover at `at`, drawing from a stream of its own for (seed, node). */
void tsim_mover_init(struct tsim_mover *m, const struct tsim_move_params *p, struct tsim_pos at,
                     uint64_t seed, uint32_t node);

/* Moves it on by `dt`. Returns whether it moved at all. */
bool tsim_mover_step(struct tsim_mover *m, const struct tsim_move_params *p, tsim_time dt);

#endif
