#ifndef TSIM_TIME_H
#define TSIM_TIME_H

#include <stdint.h>

/* Simulated time, in nanoseconds since the start of a run.
 *
 * An integer, so that two runs with the same seed order every event identically on every
 * platform. Nanoseconds because the shortest interval the simulator has to resolve is a fraction
 * of a LoRa symbol (256 us at SF7/500 kHz), and int64_t still covers 292 years. */
typedef int64_t tsim_time;

#define TSIM_NS(x) ((tsim_time)(x))
#define TSIM_US(x) ((tsim_time)(x) * 1000)
#define TSIM_MS(x) ((tsim_time)(x) * 1000000)
#define TSIM_S(x) ((tsim_time)(x) * 1000000000)

#endif
