#include "tsim/place.h"

#include <math.h>

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

/* The next waypoint and the speed to it. */
static void set_off(struct tsim_mover *m, const struct tsim_move_params *p) {
    m->to.x = p->x_min + tsim_rng_unit(&m->rng) * (p->x_max - p->x_min);
    m->to.y = p->y_min + tsim_rng_unit(&m->rng) * (p->y_max - p->y_min);
    m->speed = p->speed_min + tsim_rng_unit(&m->rng) * (p->speed_max - p->speed_min);
    m->rest = 0;
}

void tsim_mover_init(struct tsim_mover *m, const struct tsim_move_params *p, struct tsim_pos at,
                     uint64_t seed, uint32_t node) {
    *m = (struct tsim_mover){.at = at};
    tsim_rng_init(&m->rng, seed, UINT64_C(0xC7) << 56 | node);
    set_off(m, p);
}

bool tsim_mover_step(struct tsim_mover *m, const struct tsim_move_params *p, tsim_time dt) {
    bool moved = false;
    while (dt > 0) {
        if (m->rest > 0) {
            if (m->rest > dt) {
                m->rest -= dt;
                break;
            }
            dt -= m->rest;
            set_off(m, p);
        }
        double dx = m->to.x - m->at.x, dy = m->to.y - m->at.y;
        double far = hypot(dx, dy);
        double go = m->speed * (double)dt / (double)TSIM_S(1);
        moved = moved || far > 0;
        if (go < far) {
            m->at.x += dx * go / far;
            m->at.y += dy * go / far;
            break;
        }
        m->at = m->to;
        tsim_time took = (tsim_time)(far / m->speed * (double)TSIM_S(1));
        dt -= took < dt ? took : dt;
        /* Arrived: at least 1 ms, so even a box of one point lets the time run out. */
        double t = -log1p(-tsim_rng_unit(&m->rng)) * (double)p->pause;
        m->rest = t < (double)TSIM_MS(1) ? TSIM_MS(1) : t > 1e18 ? (tsim_time)1e18 : (tsim_time)t;
    }
    return moved;
}
