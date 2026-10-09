#include "tsim/metrics.h"

#include <stdlib.h>
#include <string.h>

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
    uint64_t held;
    struct tsim_phy_stats phy;
};

struct tsim_metrics {
    struct tsim_net *net;
    tsim_time deadline;
    struct latencies unicast;
    struct latencies broadcast;
    struct latencies card;
    tsim_time begun;   /* when the window began: 0 for the whole run */
    uint64_t first;    /* the first message id of the window: 1 for the whole run */
    struct base *base; /* per node, or NULL with no window begun */
    double warmup_ns;
    double routes;
    double routes_reach;
    /* Addressed frames since the window began; see struct tsim_losses. */
    uint64_t hops[2][TSIM_PHY_FATE_COUNT];
    uint64_t rivals[TSIM_PHY_FATE_COUNT][TSIM_PURPOSE_COUNT + 1];
    uint64_t progress[TSIM_PROGRESS_COUNT];
    /* The links hops are judged against, by tsim_metrics_links(): each node's neighbours are
     * adj[adj_at[i]] to adj[adj_at[i + 1]], and dist[d], once worked out, every node's hops to d.
     */
    double budget;
    uint32_t *adj_at;
    uint32_t *adj;
    uint16_t **dist;
    /* Power, once tsim_metrics_power() is first called: each node's times down, and for each the
     * start of the last data hop sent to it while down. */
    struct power *power;
    uint32_t down_now;
    tsim_time down_since; /* when down_now last changed */
    double down_ns;       /* nodes down times time, since the window began */
    struct tsim_churn churn;
};

struct span {
    tsim_time down;
    tsim_time up;       /* -1 while down */
    tsim_time last_hit; /* -1 for none */
};

struct power {
    struct span *spans;
    size_t count;
    size_t cap;
};

/* The time down that `t` falls in, or NULL: the latest first, where nearly every lookup ends. */
static struct span *down_at(const struct power *p, tsim_time t) {
    for (size_t i = p->count; i-- > 0;) {
        struct span *sp = &p->spans[i];
        if (t >= sp->down && (sp->up < 0 || t < sp->up)) {
            return sp;
        }
        if (t >= sp->down) {
            break;
        }
    }
    return NULL;
}

#define FAR UINT16_MAX

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
    if (rec->msg.id < m->first) {
        return; /* made before the window: not the window's to count */
    }
    struct latencies *l = rec->msg.card                    ? &m->card
                          : rec->msg.dst == TSIM_BROADCAST ? &m->broadcast
                                                           : &m->unicast;
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

/* Every node's hops to `d` over the links, FAR where it cannot get there; NULL when memory runs
 * out. */
static const uint16_t *hops_to(struct tsim_metrics *m, uint32_t d) {
    if (m->dist[d]) {
        return m->dist[d];
    }
    uint32_t n = tsim_net_nodes(m->net);
    uint16_t *dist = malloc(n * sizeof *dist);
    uint32_t *queue = malloc(n * sizeof *queue);
    if (!dist || !queue) {
        free(dist);
        free(queue);
        return NULL;
    }
    for (uint32_t i = 0; i < n; i++) {
        dist[i] = FAR;
    }
    uint32_t head = 0, tail = 0;
    dist[d] = 0;
    queue[tail++] = d;
    while (head < tail) {
        uint32_t at = queue[head++];
        for (uint32_t k = m->adj_at[at]; k < m->adj_at[at + 1]; k++) {
            uint32_t b = m->adj[k];
            if (dist[b] == FAR && dist[at] + 1 < FAR) {
                dist[b] = (uint16_t)(dist[at] + 1);
                queue[tail++] = b;
            }
        }
    }
    free(queue);
    m->dist[d] = dist;
    return dist;
}

/* Whether a and b are a link at the losses there are now. */
static bool linked_now(const struct tsim_metrics *m, uint32_t a, uint32_t b) {
    const struct tsim_phy *phy = tsim_net_phy(m->net);
    return tsim_phy_loss(phy, a, b) <= m->budget && tsim_phy_loss(phy, b, a) <= m->budget;
}

/* Whether they were one when tsim_metrics_links() was called: a's neighbours are in order. */
static bool linked(const struct tsim_metrics *m, uint32_t a, uint32_t b) {
    uint32_t lo = m->adj_at[a], hi = m->adj_at[a + 1];
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (m->adj[mid] == b) {
            return true;
        }
        if (m->adj[mid] < b) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

static void hop_seen(void *ctx, const struct tsim_net_hop *hop) {
    struct tsim_metrics *m = ctx;
    const struct tsim_message_record *rec = tsim_net_message(m->net, hop->carries);
    if (hop->carries && (!rec || rec->msg.dst == TSIM_BROADCAST)) {
        return;
    }
    /* By where the destination stood as the frame began, which the hop's end comes after. */
    struct span *sp = hop->carries && m->power ? down_at(&m->power[hop->to], hop->start) : NULL;
    if (sp) {
        sp->last_hit = hop->start > sp->last_hit ? hop->start : sp->last_hit;
        m->churn.hops_to_down += hop->start >= m->begun;
    }
    m->hops[hop->carries ? 0 : 1][hop->fate]++;
    if (!hop->carries) {
        return;
    }
    int rival =
        hop->rival >= 0 && hop->rival < TSIM_PURPOSE_COUNT ? hop->rival : TSIM_PURPOSE_COUNT;
    m->rivals[hop->fate][rival]++;
    if (!m->adj_at) {
        return;
    }
    const uint16_t *dist = hops_to(m, rec->msg.dst);
    if (!dist) {
        return;
    }
    enum tsim_progress p = !linked(m, hop->from, hop->to)    ? TSIM_PROGRESS_NOT_A_LINK
                           : dist[hop->to] < dist[hop->from] ? TSIM_PROGRESS_CLOSER
                           : dist[hop->to] > dist[hop->from] ? TSIM_PROGRESS_FARTHER
                                                             : TSIM_PROGRESS_LEVEL;
    m->progress[p]++;
}

bool tsim_metrics_links(struct tsim_metrics *m, const struct tsim_lora *lora, double tx_dbm) {
    const struct tsim_phy *phy = tsim_net_phy(m->net);
    uint32_t n = tsim_net_nodes(m->net);
    double floor = tsim_phy_floor_dbm(phy, lora);
    if (floor != floor) {
        return false; /* NAN: an invalid modulation */
    }
    uint32_t *at = calloc((size_t)n + 1, sizeof *at);
    uint16_t **dist = calloc(n, sizeof *dist);
    if (!at || !dist) {
        free(at);
        free(dist);
        return false;
    }
    m->budget = tx_dbm - floor;
    for (uint32_t a = 0; a < n; a++) {
        for (uint32_t b = 0; b < n; b++) {
            at[a + 1] += a != b && linked_now(m, a, b);
        }
        at[a + 1] += at[a];
    }
    uint32_t *adj = malloc((at[n] ? at[n] : 1) * sizeof *adj);
    if (!adj) {
        free(at);
        free(dist);
        return false;
    }
    for (uint32_t a = 0, k = 0; a < n; a++) {
        for (uint32_t b = 0; b < n; b++) {
            if (a != b && linked_now(m, a, b)) {
                adj[k++] = b;
            }
        }
    }
    m->adj_at = at;
    m->adj = adj;
    m->dist = dist;
    return true;
}

bool tsim_metrics_churn(struct tsim_metrics *m) {
    if (!m->power && !(m->power = calloc(tsim_net_nodes(m->net), sizeof *m->power))) {
        return false;
    }
    m->churn.present = true;
    return true;
}

bool tsim_metrics_power(struct tsim_metrics *m, uint32_t node, bool on) {
    if (!tsim_metrics_churn(m)) {
        return false;
    }
    struct power *p = &m->power[node];
    bool down = p->count && p->spans[p->count - 1].up < 0;
    if (down != on) {
        return true; /* already as told */
    }
    tsim_time now = tsim_sched_now(tsim_net_sched(m->net));
    if (!on && p->count == p->cap) {
        size_t cap = p->cap ? 2 * p->cap : 4;
        struct span *grown = realloc(p->spans, cap * sizeof *grown);
        if (!grown) {
            return false;
        }
        p->spans = grown;
        p->cap = cap;
    }
    m->down_ns +=
        (double)m->down_now * (double)(now - (m->down_since > m->begun ? m->down_since : m->begun));
    m->down_since = now;
    if (on) {
        p->spans[p->count - 1].up = now;
        m->down_now--;
    } else {
        p->spans[p->count++] = (struct span){.down = now, .up = -1, .last_hit = -1};
        m->churn.downs += now >= m->begun;
        m->down_now++;
    }
    return true;
}

struct tsim_metrics *tsim_metrics_create(struct tsim_net *net, tsim_time deadline) {
    struct tsim_metrics *m = calloc(1, sizeof *m);
    if (!m) {
        return NULL;
    }
    m->net = net;
    m->deadline = deadline;
    m->first = 1;
    m->routes = -1;
    m->routes_reach = -1;
    tsim_net_observe(net, delivered, m);
    tsim_net_observe_hops(net, hop_seen, m);
    return m;
}

void tsim_metrics_destroy(struct tsim_metrics *m) {
    if (!m) {
        return;
    }
    tsim_net_observe(m->net, NULL, NULL);
    tsim_net_observe_hops(m->net, NULL, NULL);
    if (m->dist) {
        for (uint32_t d = 0; d < tsim_net_nodes(m->net); d++) {
            free(m->dist[d]);
        }
    }
    free(m->dist);
    free(m->adj_at);
    free(m->adj);
    free(m->base);
    for (uint32_t i = 0; m->power && i < tsim_net_nodes(m->net); i++) {
        free(m->power[i].spans);
    }
    free(m->power);
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
            void *r = tsim_net_routing(net, src); /* NULL for a node powered down */
            if (dst == src || !r || !routing->next_hop(r, dst, &next)) {
                continue;
            }
            held++;
            do {
                at = next;
            } while (at != dst && ++hops < n - 1 && at < n && (r = tsim_net_routing(net, at)) &&
                     routing->next_hop(r, dst, &next));
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
        base[i].held = tsim_net_stats(m->net, i)->held;
        base[i].phy = *tsim_phy_stats(phy, i);
        m->warmup_ns += (double)tsim_ledger_airtime(&base[i].ledger);
    }
    m->base = base;
    m->begun = tsim_sched_now(tsim_net_sched(m->net));
    /* By id, not time: a message made earlier at this same instant is still before the window. */
    m->first = tsim_net_message_count(m->net) + 1;
    /* Every delivery so far was of a message made before the window. */
    m->unicast = (struct latencies){0};
    m->broadcast = (struct latencies){0};
    m->card = (struct latencies){0};
    memset(m->hops, 0, sizeof m->hops);
    memset(m->rivals, 0, sizeof m->rivals);
    memset(m->progress, 0, sizeof m->progress);
    m->churn = (struct tsim_churn){.present = m->churn.present};
    m->down_ns = 0;
    count_routes(m);
}

static void finish(struct tsim_delivery *d, const struct latencies *l) {
    d->on_time = l->on_time;
    d->latency_p50 = percentile(l, 0.50);
    d->latency_p95 = percentile(l, 0.95);
    d->latency_max = l->max;
}

/* Books a unicast message's one fate. */
static void fate(const struct tsim_metrics *m, const struct tsim_message_record *rec,
                 struct tsim_losses *l, double *wait_ns, double *transit_ns, uint64_t *late_sent) {
    const struct tsim_message_drop *drop = &rec->drop;
    for (int c = 0; c < TSIM_DROP_COUNT; c++) {
        l->drops[c] += rec->drops[c];
    }
    if (rec->delivered) {
        if (rec->first - rec->msg.created <= m->deadline) {
            l->on_time++;
            return;
        }
        l->late++;
        if (rec->sent >= 0) {
            *wait_ns += (double)(rec->sent - rec->msg.created);
            *transit_ns += (double)(rec->first - rec->sent);
            (*late_sent)++;
        }
    } else if (rec->refused) {
        l->refused++;
    } else if (drop->cause < TSIM_DROP_COUNT && !drop->next_held) {
        enum tsim_place place = drop->node == rec->msg.src   ? TSIM_PLACE_SOURCE
                                : drop->next == rec->msg.dst ? TSIM_PLACE_LAST_HOP
                                                             : TSIM_PLACE_PARTWAY;
        l->dropped[drop->cause][place]++;
    } else if (drop->cause < TSIM_DROP_COUNT) {
        l->unheard++;
    } else if (rec->finished) {
        l->vanished++;
    } else {
        l->pending++;
    }
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
    double wait_ns = 0, transit_ns = 0;
    uint64_t late_sent = 0;
    for (uint64_t id = m->first; id <= count; id++) {
        const struct tsim_message_record *rec = tsim_net_message(net, id);
        struct tsim_delivery *d = rec->msg.card                    ? &r->card
                                  : rec->msg.dst == TSIM_BROADCAST ? &r->broadcast
                                                                   : &r->unicast;
        d->messages++;
        d->refused += rec->refused;
        d->wanted += rec->wanted;
        d->delivered += rec->delivered;
        if (rec->msg.dst != TSIM_BROADCAST) {
            fate(m, rec, &r->losses, &wait_ns, &transit_ns, &late_sent);
            if (m->power && down_at(&m->power[rec->msg.dst], rec->msg.created)) {
                r->churn.to_down++;
            }
        }
    }
    if (m->power) {
        tsim_time now = tsim_sched_now(tsim_net_sched(net));
        struct tsim_churn c = m->churn;
        for (uint32_t i = 0; i < tsim_net_nodes(net); i++) {
            for (size_t k = 0; k < m->power[i].count; k++) {
                const struct span *sp = &m->power[i].spans[k];
                if (sp->down >= m->begun && sp->last_hit >= 0) {
                    c.routed_to++;
                    c.repair_s += (double)(sp->last_hit - sp->down) / 1e9;
                }
            }
        }
        tsim_time since = m->down_since > m->begun ? m->down_since : m->begun;
        double down_ns = m->down_ns + (double)m->down_now * (double)(now - since);
        r->churn.present = true;
        r->churn.downs = c.downs;
        r->churn.hops_to_down = c.hops_to_down;
        r->churn.routed_to = c.routed_to;
        r->churn.repair_s = c.routed_to ? c.repair_s / (double)c.routed_to : 0;
        r->churn.down_mean = r->elapsed > 0 ? down_ns / (double)r->elapsed : 0;
    }
    if (late_sent) {
        r->losses.late_wait_s = wait_ns / (double)late_sent / 1e9;
        r->losses.late_transit_s = transit_ns / (double)late_sent / 1e9;
    }
    memcpy(r->losses.hops, m->hops, sizeof m->hops);
    memcpy(r->losses.rivals, m->rivals, sizeof m->rivals);
    memcpy(r->losses.progress, m->progress, sizeof m->progress);
    finish(&r->unicast, &m->unicast);
    finish(&r->broadcast, &m->broadcast);
    finish(&r->card, &m->card);

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
        r->duty_held += tsim_net_stats(net, i)->held - (b ? b->held : 0);
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
