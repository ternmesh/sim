#include "tsim/place.h"

#include "tsim/rng.h"

void tsim_place_uniform(struct tsim_pos *pos, uint32_t nodes, double width_m, double height_m,
                        uint64_t seed) {
    for (uint32_t i = 0; i < nodes; i++) {
        struct tsim_rng rng;
        tsim_rng_init(&rng, seed, UINT64_C(0xC2) << 56 | i);
        double x = tsim_rng_unit(&rng) * width_m;
        double y = tsim_rng_unit(&rng) * height_m;
        pos[i] = (struct tsim_pos){x, y};
    }
}

void tsim_place_grid(struct tsim_pos *pos, uint32_t nodes, double spacing_m) {
    uint32_t cols = 1;
    while ((uint64_t)cols * cols < nodes) {
        cols++;
    }
    for (uint32_t i = 0; i < nodes; i++) {
        pos[i] = (struct tsim_pos){(double)(i % cols) * spacing_m, (double)(i / cols) * spacing_m};
    }
}

void tsim_place_line(struct tsim_pos *pos, uint32_t nodes, double spacing_m) {
    for (uint32_t i = 0; i < nodes; i++) {
        pos[i] = (struct tsim_pos){(double)i * spacing_m, 0.0};
    }
}
