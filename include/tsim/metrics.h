#ifndef TSIM_METRICS_H
#define TSIM_METRICS_H

#include <stdbool.h>
#include <stdint.h>

#include "tsim/net.h"
#include "tsim/node.h"
#include "tsim/phy.h"
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
    uint64_t messages;  /* originated in the window, including those refused */
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

/* How candidate 3's links and routes held up over the window (MSH-54). Only tsim_scenario_run()
 * fills it, and only for distvec; `present` is false otherwise. Causes are in the order of enum
 * tsim_distvec_down: ihu, rate, silent, hop, timeout. */
#define TSIM_HEALTH_CAUSES 5
struct tsim_route_health {
    bool present;
    double down_per_h[TSIM_HEALTH_CAUSES];   /* usable links going out of use, all nodes */
    double strong_per_h[TSIM_HEALTH_CAUSES]; /* of those, links the node measured strong */
    double outages_per_h;     /* routes relays lost to destinations they announce, all relays */
    double outage_mean_s;     /* how long such a destination stayed without a route, on average */
    double unrouted_mean;     /* how many a relay was without a route to, on average */
    double urgent_mean;       /* changed routes waiting on a relay's urgent list, on average */
    double relay_reach_begin; /* relay to relay pairs whose routes, followed, get there: as the */
    double relay_reach_end;   /* window begins, and as the run ends */
    /* Repair, per hour, all nodes: seqno and route requests made, starved destinations given up
     * on, seqs raised to answer a request. And as the run ends, per relay: destinations without
     * a route that it holds only infeasible routes to, and that it holds none to. */
    double seqno_requests_per_h;
    double route_requests_per_h;
    double gave_up_per_h;
    double seq_raised_per_h;
    double unrouted_infeasible;
    double unrouted_empty;
};

struct tsim_report {
    tsim_time elapsed; /* the measured window so far, which every duty cycle is a fraction of */
    tsim_time deadline;
    tsim_time warmup; /* the run before the window: see tsim_metrics_begin() */
    struct tsim_delivery unicast;
    struct tsim_delivery broadcast;

    uint64_t frames[TSIM_PURPOSE_COUNT];
    /* Seconds of airtime, every node, by purpose, over the window: a frame still on the air counts
     * for the part of it sent. Doubles, because one node's airtime is no longer than the run but
     * the sum over nodes can be longer than a tsim_time holds. Summed in nanoseconds, so exact up
     * to 2^53 ns, which is 104 days. */
    double airtime_s[TSIM_PURPOSE_COUNT];
    double airtime_total_s;

    /* On-time deliveries, unicast and broadcast, per second of total airtime; 0 with no airtime. */
    double on_time_per_airtime_s;

    double duty_max;        /* the busiest node's airtime in the window over elapsed */
    uint32_t duty_max_node; /* the lowest-numbered node with that duty cycle */
    double duty_mean;

    /* What starting up cost, and how far it got: airtime before the window, every node, and, as
     * the window began, the share of ordered pairs of nodes where the source held a route to the
     * destination, and where following each node's route in turn would get there. The shares are
     * -1 for routing that keeps no routes to ask, and with no window begun. */
    double warmup_airtime_s;
    double routes;
    double routes_reach;

    uint64_t queue_dropped; /* frames refused for a full queue, every node */
    uint64_t rx_ok;         /* receptions by every radio: see struct tsim_phy_stats */
    uint64_t rx_lost;
    uint64_t rx_preempted;
    uint64_t rx_aborted;
    uint64_t rx_missed;

    /* The links the run had to work with, which a result means little without: the same protocol
     * ranks differently where every node hears a dozen others and where it hears hundreds.
     * tsim_metrics_report() leaves it zero, since only the scenario knows the radio settings;
     * tsim_scenario_run() measures it at the scenario's radio.* modulation and power. */
    struct tsim_phy_links links;

    struct tsim_route_health health;
};

struct tsim_metrics;

/* Starts watching `net` for deliveries, which must outlive it. Create it before the first message
 * is originated: a delivery it did not see has no latency. Returns NULL when memory runs out. */
struct tsim_metrics *tsim_metrics_create(struct tsim_net *net, tsim_time deadline);

/* Stops watching and frees it. */
void tsim_metrics_destroy(struct tsim_metrics *metrics);

/* Starts the measured window now, once its routing has had its warmup: from here on are the
 * airtime, frames, duty cycles, queue drops and receptions reported, so a protocol that takes hours
 * to settle can be judged settled; the airtime before goes in warmup_airtime_s, and the routes held
 * now in routes and routes_reach. Only the messages originated after this call are counted,
 * wherever their deliveries fall, so traffic during the warmup loads the network without being
 * judged. The airtime still spent on its messages after now is the window's: airtime is not told
 * apart by message. Called at most once; without it, the window is the whole run. */
void tsim_metrics_begin(struct tsim_metrics *metrics);

/* The run so far. */
void tsim_metrics_report(const struct tsim_metrics *metrics, struct tsim_report *report);

#endif
