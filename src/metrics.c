#include "tsim/metrics.h"

#include <stdlib.h>

#include "tsim/phy.h"
#include "tsim/sched.h"

/* Latencies go into a log-linear histogram rather than a list: a 1000-node run of broadcasts makes
 * millions of deliveries. Below 64 ns a bucket is one value; above, each power of two is split in
 * 64, so a bucket's lower bound is within 1/64 of anything in it. */
enum { SUB = 64, BUCKETS = SUB + 57 * SUB };

struct latencies {
    uint64_t count[BUCKETS];
    uint64_t total;
    tsim_time max;
    uint64_t on_time;
};

/* What a node had spent and received when the window began, to take from what it has at the end. */
struct base {
    struct tsim_ledger ledger;
    uint64_t dropped;
    struct tsim_phy_stats phy;
};

struct tsim_metrics {
    struct tsim_net *net;
    tsim_time deadline;
    struct latencies unicast;
    struct latencies broadcast;
    tsim_time begun;   /* when the window began: 0 for the whole run */
    struct base *base; /* per node, or NULL with no window begun */
    double warmup_ns;
    double routes;
    double routes_reach;
};

static int msb(uint64_t v) {
    int r = 0;
    while (v >>= 1) {
        r++;
    }
    return r;
}

static size_t bucket_of(tsim_time t) {
    uint64_t v = (uint64_t)t;
    if (v < SUB) {
        return (size_t)v;
    }
    int e = msb(v) - 6; /* v >> e is in [64, 128) */
    return SUB + (size_t)e * SUB + (size_t)((v >> e) - SUB);
}

static tsim_time bucket_floor(size_t b) {
    if (b < SUB) {
        return (tsim_time)b;
    }
    size_t e = (b - SUB) / SUB;
    return (tsim_time)((uint64_t)(SUB + (b - SUB) % SUB) << e);
}

/* The smallest bucket floor below which fewer than `rank` deliveries fall: nearest rank. */
static tsim_time percentile(const struct latencies *l, double p) {
    if (l->total == 0) {
        return 0;
    }
    uint64_t rank = (uint64_t)((double)l->total * p);
    if ((double)rank < (double)l->total * p || rank == 0) {
        rank++;
    }
    uint64_t seen = 0;
    for (size_t b = 0; b < BUCKETS; b++) {
        seen += l->count[b];
        if (seen >= rank) {
            return bucket_floor(b);
        }
    }
    return l->max;
}

static void delivered(void *ctx, const struct tsim_message_record *rec, uint32_t node) {
    (void)node;
    struct tsim_metrics *m = ctx;
    struct latencies *l = rec->msg.dst == TSIM_BROADCAST ? &m->broadcast : &m->unicast;
    tsim_time latency = tsim_sched_now(tsim_net_sched(m->net)) - rec->msg.created;
    l->count[bucket_of(latency)]++;
    l->total++;
    if (latency > l->max) {
        l->max = latency;
    }
    if (latency <= m->deadline) {
        l->on_time++;
    }
}

struct tsim_metrics *tsim_metrics_create(struct tsim_net *net, tsim_time deadline) {
    struct tsim_metrics *m = calloc(1, sizeof *m);
    if (!m) {
        return NULL;
    }
    m->net = net;
    m->deadline = deadline;
    m->routes = -1;
    m->routes_reach = -1;
    tsim_net_observe(net, delivered, m);
    return m;
}

void tsim_metrics_destroy(struct tsim_metrics *m) {
    if (!m) {
        return;
    }
    tsim_net_observe(m->net, NULL, NULL);
    free(m->base);
    free(m);
}

/* The shares of ordered pairs where the source holds a route, and where the routes followed from
 * node to node reach the destination - in at most n - 1 hops, so a loop counts as not reaching. */
static void count_routes(struct tsim_metrics *m) {
    struct tsim_net *net = m->net;
    const struct tsim_routing *routing = tsim_net_routing_plugin(net);
    uint32_t n = tsim_net_nodes(net);
    if (!routing->next_hop || n < 2) {
        return;
    }
    uint64_t held = 0, reach = 0;
    for (uint32_t src = 0; src < n; src++) {
        for (uint32_t dst = 0; dst < n; dst++) {
            uint32_t at = src, next, hops = 0;
            if (dst == src || !routing->next_hop(tsim_net_routing(net, src), dst, &next)) {
                continue;
            }
            held++;
            do {
                at = next;
            } while (at != dst && ++hops < n - 1 && at < n &&
                     routing->next_hop(tsim_net_routing(net, at), dst, &next));
            reach += at == dst;
        }
    }
    double pairs = (double)n * (double)(n - 1);
    m->routes = (double)held / pairs;
    m->routes_reach = (double)reach / pairs;
}

void tsim_metrics_begin(struct tsim_metrics *m) {
    if (m->base) {
        return;
    }
    uint32_t n = tsim_net_nodes(m->net);
    struct base *base = calloc(n, sizeof *base);
    if (!base) {
        return; /* out of memory: the window stays the whole run */
    }
    const struct tsim_phy *phy = tsim_net_phy(m->net);
    for (uint32_t i = 0; i < n; i++) {
        tsim_net_ledger_now(m->net, i, &base[i].ledger);
        base[i].dropped = tsim_net_stats(m->net, i)->dropped;
        base[i].phy = *tsim_phy_stats(phy, i);
        m->warmup_ns += (double)tsim_ledger_airtime(&base[i].ledger);
    }
    m->base = base;
    m->begun = tsim_sched_now(tsim_net_sched(m->net));
    count_routes(m);
}

static void finish(struct tsim_delivery *d, const struct latencies *l) {
    d->on_time = l->on_time;
    d->latency_p50 = percentile(l, 0.50);
    d->latency_p95 = percentile(l, 0.95);
    d->latency_max = l->max;
}

void tsim_metrics_report(const struct tsim_metrics *m, struct tsim_report *r) {
    struct tsim_net *net = m->net;
    *r = (struct tsim_report){
        .elapsed = tsim_sched_now(tsim_net_sched(net)) - m->begun,
        .deadline = m->deadline,
        .warmup = m->begun,
        .warmup_airtime_s = m->warmup_ns / 1e9,
        .routes = m->routes,
        .routes_reach = m->routes_reach,
    };

    uint64_t count = tsim_net_message_count(net);
    for (uint64_t id = 1; id <= count; id++) {
        const struct tsim_message_record *rec = tsim_net_message(net, id);
        struct tsim_delivery *d = rec->msg.dst == TSIM_BROADCAST ? &r->broadcast : &r->unicast;
        d->messages++;
        d->refused += rec->refused;
        d->wanted += rec->wanted;
        d->delivered += rec->delivered;
    }
    finish(&r->unicast, &m->unicast);
    finish(&r->broadcast, &m->broadcast);

    uint32_t n = tsim_net_nodes(net);
    const struct tsim_phy *phy = tsim_net_phy(net);
    tsim_time busiest = -1;
    double airtime_ns[TSIM_PURPOSE_COUNT] = {0};
    double total_ns = 0;
    for (uint32_t i = 0; i < n; i++) {
        struct tsim_ledger now;
        tsim_net_ledger_now(net, i, &now);
        const struct base *b = m->base ? &m->base[i] : NULL;
        if (b) {
            for (int p = 0; p < TSIM_PURPOSE_COUNT; p++) {
                now.frames[p] -= b->ledger.frames[p];
                now.airtime[p] -= b->ledger.airtime[p];
            }
        }
        const struct tsim_ledger *ledger = &now;
        for (int p = 0; p < TSIM_PURPOSE_COUNT; p++) {
            r->frames[p] += ledger->frames[p];
            airtime_ns[p] += (double)ledger->airtime[p];
        }
        tsim_time air = tsim_ledger_airtime(ledger);
        total_ns += (double)air;
        if (air > busiest) {
            busiest = air;
            r->duty_max_node = i;
        }
        r->queue_dropped += tsim_net_stats(net, i)->dropped - (b ? b->dropped : 0);
        const struct tsim_phy_stats *ps = tsim_phy_stats(phy, i);
        r->rx_ok += ps->rx_ok - (b ? b->phy.rx_ok : 0);
        r->rx_lost += ps->rx_lost - (b ? b->phy.rx_lost : 0);
        r->rx_preempted += ps->rx_preempted - (b ? b->phy.rx_preempted : 0);
        r->rx_aborted += ps->rx_aborted - (b ? b->phy.rx_aborted : 0);
        r->rx_missed += ps->rx_missed - (b ? b->phy.rx_missed : 0);
    }
    for (int p = 0; p < TSIM_PURPOSE_COUNT; p++) {
        r->airtime_s[p] = airtime_ns[p] / 1e9;
    }
    r->airtime_total_s = total_ns / 1e9;
    if (r->elapsed > 0) {
        r->duty_max = (double)busiest / (double)r->elapsed;
        r->duty_mean = total_ns / (double)n / (double)r->elapsed;
    }
    if (total_ns > 0) {
        r->on_time_per_airtime_s =
            (double)(r->unicast.on_time + r->broadcast.on_time) / r->airtime_total_s;
    }
}
