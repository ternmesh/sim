#ifndef TSIM_PLACE_H
#define TSIM_PLACE_H

#include <stdint.h>

#include "tsim/channel.h"

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

#endif
