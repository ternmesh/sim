#include "tsim/phy.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Frames live in a queue ordered by id, which is also the order they started. A frame is kept
 * after it ends for as long as it overlaps a frame still on the air, because that frame's
 * receivers will need its energy when they decide whether it survived. */

enum radio_state { LISTEN, RECEIVE, TRANSMIT, RETUNE };

struct air {
    struct tsim_frame f;
    bool on_air;
};

struct node {
    struct tsim_phy *phy;
    uint32_t index;
    enum radio_state state;
    uint16_t channel;
    struct tsim_lora tuned;
    uint64_t frame;         /* RECEIVE: the frame it is on; TRANSMIT: the frame it is sending */
    double frame_dbm;       /* RECEIVE: that frame's power here */
    tsim_time since;        /* RECEIVE: when it started listening to that frame */
    tsim_time lock_at;      /* RECEIVE: until then, a louder frame can take the receiver */
    struct tsim_event wake; /* TRANSMIT: the frame's end; RETUNE: listening again */
    struct tsim_phy_stats stats;
};

struct outcome {
    uint32_t node;
    double rssi_dbm;
    bool ok;
};

struct tsim_phy {
    struct tsim_sched *sched;
    struct tsim_phy_params params;
    struct tsim_phy_hooks hooks;
    uint32_t n;
    struct node *nodes;
    double *loss; /* n * n, symmetric */
    struct outcome *outcomes;

    struct air *air;
    size_t head;
    size_t count;
    size_t cap;
    uint64_t first_id; /* the id of air[head] */
    uint64_t next_id;
};

static double mw(double dbm) { return pow(10.0, dbm / 10.0); }

static double noise_dbm(const struct tsim_phy *phy, uint32_t bw_hz) {
    return -174.0 + 10.0 * log10((double)bw_hz) + phy->params.noise_figure_db;
}

static struct air *find(struct tsim_phy *phy, uint64_t id) {
    return &phy->air[phy->head + (id - phy->first_id)];
}

static double rx_dbm(const struct tsim_phy *phy, const struct tsim_frame *f, uint32_t node) {
    return f->tx_dbm - phy->loss[(size_t)f->src * phy->n + node];
}

static bool tuned_to(const struct node *nd, const struct tsim_frame *f) {
    return nd->channel == f->channel && nd->tuned.sf == f->lora.sf &&
           nd->tuned.bw_hz == f->lora.bw_hz;
}

static bool decodable(const struct tsim_phy *phy, const struct tsim_frame *f, double dbm) {
    return dbm - noise_dbm(phy, f->lora.bw_hz) >= phy->params.snr_min_db[f->lora.sf - TSIM_SF_MIN];
}

/* Whether a receiver that starts listening at `now` can still lock on to the frame: it needs
 * lock_symbols of preamble, which on air is the programmed length plus 4.25 symbols. A preamble
 * shorter than lock_symbols can never be locked on, even from its first symbol. */
static bool can_lock(const struct tsim_phy *phy, const struct tsim_frame *f, tsim_time now) {
    int64_t quarters = 4 * (int64_t)f->lora.preamble + 17 - 4 * (int64_t)phy->params.lock_symbols;
    return quarters >= 0 && now <= f->start + tsim_lora_symbol(&f->lora) * quarters / 4;
}

static void sync_to(struct node *nd, const struct tsim_frame *f, double dbm) {
    tsim_time now = tsim_sched_now(nd->phy->sched);
    nd->state = RECEIVE;
    nd->frame = f->id;
    nd->frame_dbm = dbm;
    nd->since = now;
    nd->lock_at = now + tsim_lora_symbol(&f->lora) * (tsim_time)nd->phy->params.lock_symbols;
}

/* Starts listening, catching the loudest matching frame whose preamble is still long enough to
 * lock on. */
static void listen(struct node *nd) {
    struct tsim_phy *phy = nd->phy;
    tsim_time now = tsim_sched_now(phy->sched);
    const struct tsim_frame *best = NULL;
    double best_dbm = 0;
    nd->state = LISTEN;
    for (size_t i = 0; i < phy->count; i++) {
        const struct air *a = &phy->air[phy->head + i];
        if (!a->on_air || a->f.src == nd->index || !tuned_to(nd, &a->f) ||
            !can_lock(phy, &a->f, now)) {
            continue;
        }
        double dbm = rx_dbm(phy, &a->f, nd->index);
        if (decodable(phy, &a->f, dbm) && (!best || dbm > best_dbm)) {
            best = &a->f;
            best_dbm = dbm;
        }
    }
    if (best) {
        sync_to(nd, best, best_dbm);
    }
}

static void retune_done(struct tsim_sched *sched, void *ctx) {
    (void)sched;
    listen(ctx);
}

static void begin_retune(struct node *nd) {
    struct tsim_phy *phy = nd->phy;
    if (nd->state == RETUNE) {
        tsim_sched_cancel(phy->sched, nd->wake);
    }
    if (phy->params.retune <= 0) {
        listen(nd);
        return;
    }
    nd->state = RETUNE;
    nd->wake = tsim_sched_after(phy->sched, phy->params.retune, retune_done, nd);
}

/* Whether a frame received at `dbm` by `node` survives everything else that overlapped it while
 * the node was listening to it - from `since`, which is the frame's start unless the node caught
 * it part-way through the preamble. What was on the air before then never reached the
 * demodulator, so it neither helps nor hurts. */
static bool survives(const struct tsim_phy *phy, const struct tsim_frame *f, uint32_t node,
                     double dbm, tsim_time since) {
    double energy[TSIM_SF_COUNT] = {0};
    for (size_t i = 0; i < phy->count; i++) {
        const struct tsim_frame *g = &phy->air[phy->head + i].f;
        if (g->id == f->id || g->channel != f->channel) {
            continue;
        }
        tsim_time lo = g->start > since ? g->start : since;
        tsim_time hi = g->end < f->end ? g->end : f->end;
        if (hi <= lo) {
            continue;
        }
        int sf = g->lora.bw_hz == f->lora.bw_hz ? g->lora.sf : f->lora.sf;
        energy[sf - TSIM_SF_MIN] += mw(rx_dbm(phy, g, node)) * (double)(hi - lo);
    }
    double signal = mw(dbm) * (double)(f->end - since);
    const double *threshold = phy->params.isolation_db[f->lora.sf - TSIM_SF_MIN];
    for (int k = 0; k < TSIM_SF_COUNT; k++) {
        if (energy[k] > 0 && 10.0 * log10(signal / energy[k]) < threshold[k]) {
            return false;
        }
    }
    return true;
}

/* Drops ended frames that no frame still on the air overlaps. */
static void prune(struct tsim_phy *phy) {
    tsim_time cutoff = INT64_MAX;
    for (size_t i = 0; i < phy->count; i++) {
        if (phy->air[phy->head + i].on_air) {
            cutoff = phy->air[phy->head + i].f.start;
            break;
        }
    }
    while (phy->count > 0 && !phy->air[phy->head].on_air && phy->air[phy->head].f.end <= cutoff) {
        phy->head++;
        phy->count--;
        phy->first_id++;
    }
    if (phy->count == 0) {
        phy->head = 0;
    }
}

static void frame_end(struct tsim_sched *sched, void *ctx) {
    (void)sched;
    struct node *tx = ctx;
    struct tsim_phy *phy = tx->phy;
    struct air *a = find(phy, tx->frame);
    a->on_air = false;
    struct tsim_frame f = a->f;

    /* Decide every reception first, then let the radios listen again, then run the hooks, so the
     * outcome does not depend on which node a hook belongs to. */
    size_t done = 0;
    for (uint32_t i = 0; i < phy->n; i++) {
        struct node *nd = &phy->nodes[i];
        if (nd->state != RECEIVE || nd->frame != f.id) {
            continue;
        }
        bool ok = survives(phy, &f, i, nd->frame_dbm, nd->since);
        if (ok) {
            nd->stats.rx_ok++;
        } else {
            nd->stats.rx_lost++;
        }
        phy->outcomes[done++] = (struct outcome){i, nd->frame_dbm, ok};
        nd->state = LISTEN;
    }

    if (tuned_to(tx, &f)) {
        listen(tx);
    } else {
        tx->state = LISTEN;
        begin_retune(tx);
    }
    for (size_t k = 0; k < done; k++) {
        listen(&phy->nodes[phy->outcomes[k].node]);
    }
    prune(phy);

    if (phy->hooks.rx) {
        double noise = noise_dbm(phy, f.lora.bw_hz);
        for (size_t k = 0; k < done; k++) {
            const struct outcome *o = &phy->outcomes[k];
            if (o->ok) {
                phy->hooks.rx(phy->hooks.ctx, o->node, &f, o->rssi_dbm, o->rssi_dbm - noise);
            }
        }
    }
    if (phy->hooks.tx_done) {
        phy->hooks.tx_done(phy->hooks.ctx, tx->index, &f);
    }
}

static struct air *push(struct tsim_phy *phy) {
    if (phy->head + phy->count == phy->cap) {
        if (phy->head > 0) {
            memmove(phy->air, phy->air + phy->head, phy->count * sizeof *phy->air);
            phy->head = 0;
        }
        if (phy->count == phy->cap) {
            size_t cap = phy->cap ? phy->cap * 2 : 64;
            struct air *grown = realloc(phy->air, cap * sizeof *grown);
            if (!grown) {
                return NULL;
            }
            phy->air = grown;
            phy->cap = cap;
        }
    }
    if (phy->count == 0) {
        phy->first_id = phy->next_id;
    }
    phy->count++;
    return &phy->air[phy->head + phy->count - 1];
}

struct tsim_phy_params tsim_phy_defaults(void) {
    return (struct tsim_phy_params){
        .noise_figure_db = 6.0,
        .snr_min_db = {-7.5, -10.0, -12.5, -15.0, -17.5, -20.0},
        .isolation_db =
            {
                {6, -16, -18, -19, -19, -20},
                {-24, 6, -20, -22, -22, -22},
                {-27, -27, 6, -23, -25, -25},
                {-30, -30, -30, 6, -26, -28},
                {-33, -33, -33, -33, 6, -29},
                {-36, -36, -36, -36, -36, 6},
            },
        .capture_db = 6.0,
        .lock_symbols = 5,
        .retune = TSIM_MS(1),
    };
}

struct tsim_phy *tsim_phy_create(struct tsim_sched *sched, const struct tsim_phy_params *params,
                                 uint32_t nodes, uint16_t channel,
                                 const struct tsim_lora *listen_on, struct tsim_phy_hooks hooks) {
    if (nodes == 0 || !tsim_lora_valid(listen_on)) {
        return NULL;
    }
    struct tsim_phy *phy = calloc(1, sizeof *phy);
    if (!phy) {
        return NULL;
    }
    phy->sched = sched;
    phy->params = *params;
    phy->hooks = hooks;
    phy->n = nodes;
    phy->next_id = 1;
    phy->first_id = 1;
    phy->nodes = calloc(nodes, sizeof *phy->nodes);
    phy->loss = malloc((size_t)nodes * nodes * sizeof *phy->loss);
    phy->outcomes = malloc(nodes * sizeof *phy->outcomes);
    if (!phy->nodes || !phy->loss || !phy->outcomes) {
        tsim_phy_destroy(phy);
        return NULL;
    }
    for (size_t i = 0; i < (size_t)nodes * nodes; i++) {
        phy->loss[i] = INFINITY;
    }
    for (uint32_t i = 0; i < nodes; i++) {
        phy->nodes[i] = (struct node){
            .phy = phy,
            .index = i,
            .state = LISTEN,
            .channel = channel,
            .tuned = *listen_on,
        };
    }
    return phy;
}

void tsim_phy_destroy(struct tsim_phy *phy) {
    if (!phy) {
        return;
    }
    /* A frame still on the air or a retune under way has an event in the scheduler that points
     * into the nodes, and the scheduler may outlive the medium. */
    for (uint32_t i = 0; phy->nodes && i < phy->n; i++) {
        struct node *nd = &phy->nodes[i];
        if (nd->state == TRANSMIT || nd->state == RETUNE) {
            tsim_sched_cancel(phy->sched, nd->wake);
        }
    }
    free(phy->nodes);
    free(phy->loss);
    free(phy->outcomes);
    free(phy->air);
    free(phy);
}

void tsim_phy_set_loss(struct tsim_phy *phy, uint32_t a, uint32_t b, double loss_db) {
    phy->loss[(size_t)a * phy->n + b] = loss_db;
    phy->loss[(size_t)b * phy->n + a] = loss_db;
}

double tsim_phy_loss(const struct tsim_phy *phy, uint32_t a, uint32_t b) {
    return phy->loss[(size_t)a * phy->n + b];
}

void tsim_phy_set_losses(struct tsim_phy *phy, const struct tsim_channel_params *channel,
                         const struct tsim_pos *pos) {
    for (uint32_t a = 0; a < phy->n; a++) {
        for (uint32_t b = a + 1; b < phy->n; b++) {
            tsim_phy_set_loss(phy, a, b, tsim_channel_loss(channel, a, pos[a], b, pos[b]));
        }
    }
}

uint64_t tsim_phy_transmit(struct tsim_phy *phy, uint32_t node, uint16_t channel,
                           const struct tsim_lora *lora, uint32_t len, double tx_dbm,
                           void *payload) {
    if (node >= phy->n || phy->nodes[node].state == TRANSMIT) {
        return 0;
    }
    tsim_time airtime = tsim_lora_airtime(lora, len);
    if (airtime < 0) {
        return 0;
    }
    struct air *a = push(phy);
    if (!a) {
        return 0;
    }
    tsim_time now = tsim_sched_now(phy->sched);
    a->on_air = true;
    a->f = (struct tsim_frame){
        .id = phy->next_id++,
        .src = node,
        .channel = channel,
        .lora = *lora,
        .len = len,
        .tx_dbm = tx_dbm,
        .start = now,
        .end = now + airtime,
        .payload = payload,
    };
    const struct tsim_frame *f = &a->f;

    struct node *tx = &phy->nodes[node];
    if (tx->state == RECEIVE) {
        tx->stats.rx_aborted++;
    } else if (tx->state == RETUNE) {
        tsim_sched_cancel(phy->sched, tx->wake);
    }
    tx->state = TRANSMIT;
    tx->frame = f->id;
    tx->wake = tsim_sched_after(phy->sched, airtime, frame_end, tx);
    tx->stats.tx++;
    tx->stats.tx_airtime += airtime;

    for (uint32_t i = 0; i < phy->n; i++) {
        struct node *nd = &phy->nodes[i];
        if (i == node || !tuned_to(nd, f) || !can_lock(phy, f, now)) {
            continue;
        }
        if (nd->state == LISTEN) {
            double dbm = rx_dbm(phy, f, i);
            if (decodable(phy, f, dbm)) {
                sync_to(nd, f, dbm);
            }
        } else if (nd->state == RECEIVE && now < nd->lock_at) {
            double dbm = rx_dbm(phy, f, i);
            if (decodable(phy, f, dbm) && dbm >= nd->frame_dbm + phy->params.capture_db) {
                nd->stats.rx_preempted++;
                sync_to(nd, f, dbm);
            }
        }
    }
    return f->id;
}

bool tsim_phy_tune(struct tsim_phy *phy, uint32_t node, uint16_t channel,
                   const struct tsim_lora *listen_on) {
    if (node >= phy->n || !tsim_lora_valid(listen_on)) {
        return false;
    }
    struct node *nd = &phy->nodes[node];
    bool same = nd->channel == channel && nd->tuned.sf == listen_on->sf &&
                nd->tuned.bw_hz == listen_on->bw_hz;
    nd->channel = channel;
    nd->tuned = *listen_on;
    if (same || nd->state == TRANSMIT) {
        return true;
    }
    if (nd->state == RECEIVE) {
        nd->stats.rx_aborted++;
    }
    begin_retune(nd);
    return true;
}

bool tsim_phy_transmitting(const struct tsim_phy *phy, uint32_t node) {
    return phy->nodes[node].state == TRANSMIT;
}

bool tsim_phy_receiving(const struct tsim_phy *phy, uint32_t node) {
    return phy->nodes[node].state == RECEIVE;
}

bool tsim_phy_cad(const struct tsim_phy *phy, uint32_t node) {
    const struct node *nd = &phy->nodes[node];
    if (nd->state == TRANSMIT || nd->state == RETUNE) {
        return false;
    }
    for (size_t i = 0; i < phy->count; i++) {
        const struct air *a = &phy->air[phy->head + i];
        if (a->on_air && a->f.src != node && tuned_to(nd, &a->f) &&
            decodable(phy, &a->f, rx_dbm(phy, &a->f, node))) {
            return true;
        }
    }
    return false;
}

const struct tsim_phy_stats *tsim_phy_stats(const struct tsim_phy *phy, uint32_t node) {
    return &phy->nodes[node].stats;
}
