#ifndef TSIM_CHANNEL_H
#define TSIM_CHANNEL_H

#include <stdint.h>

/* The propagation channel: how much signal a link loses, in dB.
 *
 * Loss is a median path loss - log-distance unless another model is chosen - plus log-normal
 * shadowing. With log-distance:
 *
 *     L(d) = PL0 + 10 n log10(d / d0) + X,   X ~ N(0, sigma^2)
 *
 * The shadowing is correlated, because independent per-link shadowing flatters every routing
 * scheme: it makes a bad link's neighbours look like an escape. X is split into two parts.
 *
 *  - A shared part, from a Gaussian field laid over the ground: each endpoint takes the field's
 *    value where it stands, so links that start or end near each other share their luck. The
 *    field is independent between points more than two `decorrelation_m` apart.
 *  - A part of the link's own, independent of every other link.
 *
 * `node_share` is the fraction of the variance that is shared. X is the same in both directions,
 * and is a pure function of the seed, the two node numbers and their positions, so a scenario
 * repeats and two scenarios with the same seed and nodes see the same terrain.
 *
 * The defaults are Petäjäjärvi et al., "On the coverage of LPWANs: range evaluation and channel
 * attenuation model for LoRa technology" (ITST 2015), an outdoor 868 MHz measurement; 915 MHz
 * loses about 0.5 dB more. The correlation parameters have no single published value and are
 * meant to be swept. */

struct tsim_pos {
    double x; /* metres */
    double y;
};

/* The median loss's shape. The 3GPP models are the macro-cell formulas Meshtasticator uses (its
 * models 5 and 6, from 3GPP TR 25.996), for a base and a mobile at the same height: there for
 * reproducing its results, and as a second opinion on the log-distance fit. */
enum tsim_path_model {
    TSIM_PATH_LOG_DISTANCE, /* PL0 + 10 n log10(d / d0) */
    TSIM_PATH_3GPP_SUBURBAN,
    TSIM_PATH_3GPP_URBAN, /* 3 dB more than suburban */
};

struct tsim_channel_params {
    enum tsim_path_model model;
    double pl0_db;          /* median loss at d0 */
    double d0_m;            /* reference distance */
    double exponent;        /* path loss exponent n */
    double freq_mhz;        /* 3GPP: carrier frequency */
    double height_m;        /* 3GPP: both antennas above the ground */
    double sigma_db;        /* standard deviation of the shadowing */
    double node_share;      /* fraction of the shadowing variance that is shared, 0..1 */
    double decorrelation_m; /* spacing of the shared field */
    uint64_t seed;
};

/* Petäjäjärvi's model (PL0 128.95 dB at 1 km, n 2.32, sigma 7.8 dB), half the shadowing shared,
 * over a 100 m field. The 3GPP settings, if chosen, start at 915 MHz and 1 m. */
struct tsim_channel_params tsim_channel_default(uint64_t seed);

/* The loss without shadowing. Distances under 1 m are taken as 1 m. */
double tsim_channel_median_loss(const struct tsim_channel_params *p, double distance_m);

/* The shadowing term X for the link between nodes a and b (a != b), standing at pa and pb. */
double tsim_channel_shadowing(const struct tsim_channel_params *p, uint32_t a, struct tsim_pos pa,
                              uint32_t b, struct tsim_pos pb);

/* Median loss plus shadowing. */
double tsim_channel_loss(const struct tsim_channel_params *p, uint32_t a, struct tsim_pos pa,
                         uint32_t b, struct tsim_pos pb);

#endif
