#ifndef TSIM_METRICS_H
#define TSIM_METRICS_H

#include <stdint.h>

#include "tsim/net.h"
#include "tsim/node.h"
#include "tsim/time.h"

/* What a run is judged by.
 *
 * The headline is delivery per unit of airtime: deliveries that arrived within the deadline,
 * per second of airtime every node spent, on every purpose. A protocol that delivers more by
 * spending more does not gain on it, and neither does one that saves airtime by delivering less.
 * Beside it are the two things a headline ratio can hide: the busiest node's duty cycle, because
 * the node that runs out of airtime first is the one a regulator's limit stops, and the share of
 * what was wanted that arrived in time at all.
 *
 * Unicast and broadcast are kept apart. A broadcast has a destination in every other node, so in
 * one total it would swamp the unicasts, which are what path routing is for. A destination is
 * counted once whatever happened to the message: refused by its routing, dropped, lost or late, it
 * stays in the denominator. */

/* Deliveries of one kind of message. */
struct tsim_delivery {
    uint64_t messages;  /* originated, including those refused */
    uint64_t refused;   /* that the source's routing could not carry */
    uint64_t wanted;    /* destinations, over every message */
    uint64_t delivered; /* destinations reached, however late */
    uint64_t on_time;   /* destinations reached within the deadline */
    /* Of the deliveries, by the time from origination: the median, the 95th percentile, and the
     * longest. The percentiles are rounded down, to within 1.6%; the longest is exact. All three
     * are 0 when nothing was delivered. */
    tsim_time latency_p50;
    tsim_time latency_p95;
    tsim_time latency_max;
};

struct tsim_report {
    tsim_time elapsed; /* the run so far, which every duty cycle is a fraction of */
    tsim_time deadline;
    struct tsim_delivery unicast;
    struct tsim_delivery broadcast;

    uint64_t frames[TSIM_PURPOSE_COUNT];
    /* Seconds of airtime, every node, by purpose, up to now: a frame still on the air counts for
     * the part of it sent. Doubles, because one node's airtime is no longer than the run but the
     * sum over nodes can be longer than a tsim_time holds. Summed in nanoseconds, so exact up to
     * 2^53 ns, which is 104 days. */
    double airtime_s[TSIM_PURPOSE_COUNT];
    double airtime_total_s;

    /* On-time deliveries, unicast and broadcast, per second of total airtime; 0 with no airtime. */
    double on_time_per_airtime_s;

    double duty_max;        /* the busiest node's airtime over elapsed */
    uint32_t duty_max_node; /* the lowest-numbered node with that duty cycle */
    double duty_mean;

    uint64_t queue_dropped; /* frames refused for a full queue, every node */
    uint64_t rx_ok;         /* receptions by every radio: see struct tsim_phy_stats */
    uint64_t rx_lost;
    uint64_t rx_preempted;
    uint64_t rx_aborted;
};

struct tsim_metrics;

/* Starts watching `net` for deliveries, which must outlive it. Create it before the first message
 * is originated: a delivery it did not see has no latency. Returns NULL when memory runs out. */
struct tsim_metrics *tsim_metrics_create(struct tsim_net *net, tsim_time deadline);

/* Stops watching and frees it. */
void tsim_metrics_destroy(struct tsim_metrics *metrics);

/* The run so far. */
void tsim_metrics_report(const struct tsim_metrics *metrics, struct tsim_report *report);

#endif
