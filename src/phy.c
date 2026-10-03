#include "tsim/phy.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tsim/rng.h"

/* Frames live in a queue ordered by id, which is also the order they started. A frame is kept
 * after it ends for as long as it overlaps a frame still on the air, because that frame's
 * receivers will need its energy when they decide whether it survived. */

enum radio_state { LISTEN, RECEIVE, TRANSMIT, RETUNE };

/* Not yet decided: the marked receiver is on the frame, or may still catch it. */
#define PENDING TSIM_PHY_FATE_COUNT

struct air {
    struct tsim_frame f;
    bool on_air;
    /* For the books (tsim_phy_mark()): the tag, -1 for none, and the receiver whose fate is
     * followed, UINT32_MAX for none, with that fate so far and the frame that cost it, 0 for
     * none. */
    int8_t tag;
    uint32_t watch;
    uint8_t fate;
    uint64_t rival;
};

struct node {
    struct tsim_phy *phy;
    uint32_t index;
    enum radio_state state;
    uint16_t channel;
    struct tsim_lora tuned;
    uint64_t frame;    /* RECEIVE: the frame it is on; TRANSMIT: the frame it is sending */
    double frame_dbm;  /* RECEIVE: that frame's power here */
    tsim_time since;   /* RECEIVE: when it started listening to that frame */
    tsim_time lock_at; /* RECEIVE: until then, a louder frame can take the receiver */
    /* Frames it has given an outcome that it may yet catch, while their preambles last. */
    struct unsettled *unsettled;
    uint32_t unsettled_count;
    uint32_t unsettled_cap;
    struct tsim_event wake; /* TRANSMIT: the frame's end; RETUNE: listening again */
    struct tsim_phy_stats stats;
    tsim_time header_due;  /* RECEIVE: when that frame's header is demodulated */
    tsim_time header_hold; /* see tsim_phy_hold_header(); 0 for none */
    tsim_time header_set;  /* with a hold: when the header flag was set, or -1 */
    bool off;              /* powered down: hears nothing, sends nothing */
};

/* A frame a radio counted as missed, taken from it or cut short, but whose preamble is not yet
 * over, so the radio may catch it yet: the outcome is taken back then, and the frame counts by how
 * that reception ends. */
struct unsettled {
    uint64_t frame;
    tsim_time lock_by; /* the frame's; see lock_by() */
    uint64_t *counter; /* in the node's stats */
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
    double *loss; /* n * n: from the row's node to the column's */
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

/* A frame still kept, or NULL. */
static struct air *kept(struct tsim_phy *phy, uint64_t id) {
    return id >= phy->first_id && id - phy->first_id < phy->count ? find(phy, id) : NULL;
}

/* Books a fate for a frame at a receiver, if the frame is marked for that receiver. */
static void befall(struct tsim_phy *phy, uint64_t frame, uint32_t node, uint8_t fate,
                   uint64_t rival) {
    struct air *a = kept(phy, frame);
    if (a && a->watch == node) {
        a->fate = fate;
        a->rival = rival;
    }
}

/* The fading on one frame at one receiver: a pure function of the seed, the frame and the node,
 * drawn from a domain of its own so it never repeats a stream the seed roots elsewhere. */
static double fade(const struct tsim_phy *phy, uint64_t frame, uint32_t node) {
    struct tsim_rng r;
    tsim_rng_init(&r, phy->params.fading_seed, (uint64_t)0xC3 << 56);
    tsim_rng_init(&r, tsim_rng_next(&r), frame);
    tsim_rng_init(&r, tsim_rng_next(&r), node);
    return phy->params.fading_db * tsim_rng_normal(&r);
}

static double rx_dbm(const struct tsim_phy *phy, const struct tsim_frame *f, uint32_t node) {
    double dbm = f->tx_dbm - phy->loss[(size_t)f->src * phy->n + node];
    return phy->params.fading_db > 0 ? dbm - fade(phy, f->id, node) : dbm;
}

static bool tuned_to(const struct node *nd, const struct tsim_frame *f) {
    return !nd->off && nd->channel == f->channel && nd->tuned.sf == f->lora.sf &&
           nd->tuned.bw_hz == f->lora.bw_hz;
}

static bool decodable(const struct tsim_phy *phy, const struct tsim_frame *f, double dbm) {
    return dbm - noise_dbm(phy, f->lora.bw_hz) >= phy->params.snr_min_db[f->lora.sf - TSIM_SF_MIN];
}

/* The last instant a receiver that starts listening can still lock on to the frame: it needs
 * lock_symbols of preamble, which on air is the programmed length plus 4.25 symbols. A preamble
 * shorter than lock_symbols can never be locked on, even from its first symbol, so for one of
 * those it is before the frame begins. It is always before the frame ends. */
static tsim_time lock_by(const struct tsim_phy *phy, const struct tsim_frame *f) {
    int64_t quarters = 4 * (int64_t)f->lora.preamble + 17 - 4 * (int64_t)phy->params.lock_symbols;
    return quarters >= 0 ? f->start + tsim_lora_symbol(&f->lora) * quarters / 4 : f->start - 1;
}

static bool can_lock(const struct tsim_phy *phy, const struct tsim_frame *f, tsim_time now) {
    return now <= lock_by(phy, f);
}

/* When a frame's explicit header has been demodulated: the preamble, the 4.25 symbols of sync
 * word and start of frame, and the header's 8 symbols. */
static tsim_time header_end(const struct tsim_frame *f) {
    return f->start + tsim_lora_symbol(&f->lora) * (4 * (tsim_time)f->lora.preamble + 49) / 4;
}

/* Charges the time a reception that is ending spent on its frame. One that ends before its header
 * takes back the held header flag it would have set. */
static void count_reception(struct node *nd) {
    tsim_time now = tsim_sched_now(nd->phy->sched);
    nd->stats.rx_airtime += now - nd->since;
    if (now < nd->header_due && nd->header_set == nd->header_due) {
        nd->header_set = -1;
    }
}

static void sync_to(struct node *nd, const struct tsim_frame *f, double dbm) {
    tsim_time now = tsim_sched_now(nd->phy->sched);
    befall(nd->phy, f->id, nd->index, PENDING, 0);
    nd->state = RECEIVE;
    nd->frame = f->id;
    nd->frame_dbm = dbm;
    nd->since = now;
    nd->lock_at = now + tsim_lora_symbol(&f->lora) * (tsim_time)nd->phy->params.lock_symbols;
    nd->header_due = header_end(f);
    if (nd->header_hold > 0 && nd->header_set < 0) {
        nd->header_set = nd->header_due; /* once it gets there */
    }
}

/* Forgets the outcomes of frames that can no longer be caught: they stand. */
static void forget_settled(struct node *nd) {
    tsim_time now = tsim_sched_now(nd->phy->sched);
    uint32_t kept = 0;
    for (uint32_t k = 0; k < nd->unsettled_count; k++) {
        if (nd->unsettled[k].lock_by >= now) {
            nd->unsettled[kept++] = nd->unsettled[k];
        }
    }
    nd->unsettled_count = kept;
}

/* Counts an outcome for a frame at this node - missed, taken by a louder one, or cut short - and
 * remembers it until the frame's lock_by, so that catching the frame after all can take the
 * outcome back. Only frames that can no longer be caught are forgotten, whatever the radio is
 * tuned to meanwhile. Out of memory, the outcome stands, and a frame caught after all counts
 * twice. The fate is booked for a marked frame, with the frame that cost it. */
static void settle(struct node *nd, uint64_t frame, tsim_time frame_lock_by, uint64_t *counter,
                   uint8_t fate, uint64_t rival) {
    (*counter)++;
    befall(nd->phy, frame, nd->index, fate, rival);
    if (nd->unsettled_count == nd->unsettled_cap) {
        forget_settled(nd);
    }
    if (nd->unsettled_count == nd->unsettled_cap) {
        uint32_t cap = nd->unsettled_cap ? 2 * nd->unsettled_cap : 4;
        struct unsettled *grown = realloc(nd->unsettled, cap * sizeof *grown);
        if (!grown) {
            return;
        }
        nd->unsettled = grown;
        nd->unsettled_cap = cap;
    }
    nd->unsettled[nd->unsettled_count++] = (struct unsettled){frame, frame_lock_by, counter};
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
    if (!best) {
        forget_settled(nd); /* idle, so a short list for the next catch to look through */
        return;
    }
    /* Caught after all, its preamble not yet over: whatever it was counted as, it counts by how
     * this reception ends instead. */
    for (uint32_t k = 0; k < nd->unsettled_count; k++) {
        if (nd->unsettled[k].frame == best->id) {
            (*nd->unsettled[k].counter)--;
            nd->unsettled[k] = nd->unsettled[--nd->unsettled_count];
            break;
        }
    }
    sync_to(nd, best, best_dbm);
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

/* By LoRaSim's rule, below: how far `g` leaves `f` short of the pair's isolation threshold at
 * `node`, in dB - positive when it destroys it - or -INFINITY for a frame the rule does not count
 * against it. */
static double pairwise_shortfall(const struct tsim_phy *phy, const struct tsim_frame *f,
                                 const struct tsim_frame *g, uint32_t node, double dbm,
                                 tsim_time since) {
    if (g->id == f->id || g->channel != f->channel) {
        return -INFINITY;
    }
    tsim_time lo = g->start > since ? g->start : since;
    tsim_time hi = g->end < f->end ? g->end : f->end;
    const struct tsim_frame *later = g->start > f->start ? g : f;
    int64_t grace = (int64_t)later->lora.preamble - (int64_t)phy->params.lock_symbols;
    /* The overlap starts no earlier than the later frame does; it is forgiven only if it is over
     * by the end of that frame's grace, wherever the receiver came in. */
    tsim_time grace_end = later->start + (grace > 0 ? grace : 0) * tsim_lora_symbol(&later->lora);
    if (hi <= lo || hi <= grace_end) {
        return -INFINITY;
    }
    double other = rx_dbm(phy, g, node);
    if (!decodable(phy, g, other)) {
        return -INFINITY;
    }
    int sf = g->lora.bw_hz == f->lora.bw_hz ? g->lora.sf : f->lora.sf;
    return phy->params.isolation_db[f->lora.sf - TSIM_SF_MIN][sf - TSIM_SF_MIN] - (dbm - other);
}

/* LoRaSim's rule: each interferer on its own, at full weight, and only one the receiver could
 * decode. Overlapping no more than the first (preamble - lock_symbols) symbols of the later of the
 * two frames is no overlap: the receiver had not yet settled on it. */
static bool survives_pairwise(const struct tsim_phy *phy, const struct tsim_frame *f, uint32_t node,
                              double dbm, tsim_time since) {
    for (size_t i = 0; i < phy->count; i++) {
        if (pairwise_shortfall(phy, f, &phy->air[phy->head + i].f, node, dbm, since) > 0) {
            return false;
        }
    }
    return true;
}

/* The summed rule: the interference energy each SF brings over the reception, as
 * energy[interfering SF - 7], and, if `loudest` is given, the frame that brought the most of each
 * group, 0 for none. */
static void interference(const struct tsim_phy *phy, const struct tsim_frame *f, uint32_t node,
                         tsim_time since, double energy[TSIM_SF_COUNT],
                         uint64_t loudest[TSIM_SF_COUNT]) {
    double most[TSIM_SF_COUNT] = {0};
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
        double e = mw(rx_dbm(phy, g, node)) * (double)(hi - lo);
        energy[sf - TSIM_SF_MIN] += e;
        if (loudest && e > most[sf - TSIM_SF_MIN]) {
            most[sf - TSIM_SF_MIN] = e;
            loudest[sf - TSIM_SF_MIN] = g->id;
        }
    }
}

/* How far an SF's summed interference leaves the reception short of the pair's isolation
 * threshold, in dB - positive when it destroys it - or -INFINITY for an SF that brought none. */
static double summed_shortfall(const struct tsim_phy *phy, const struct tsim_frame *f, double dbm,
                               tsim_time since, const double energy[TSIM_SF_COUNT], int k) {
    if (!(energy[k] > 0)) {
        return -INFINITY;
    }
    double signal = mw(dbm) * (double)(f->end - since);
    return phy->params.isolation_db[f->lora.sf - TSIM_SF_MIN][k] - 10.0 * log10(signal / energy[k]);
}

/* Whether a frame received at `dbm` by `node` survives everything else that overlapped it while
 * the node was listening to it - from `since`, which is the frame's start unless the node caught
 * it part-way through the preamble. What was on the air before then never reached the
 * demodulator, so it neither helps nor hurts. */
static bool survives(const struct tsim_phy *phy, const struct tsim_frame *f, uint32_t node,
                     double dbm, tsim_time since) {
    if (phy->params.pairwise) {
        return survives_pairwise(phy, f, node, dbm, since);
    }
    double energy[TSIM_SF_COUNT] = {0};
    interference(phy, f, node, since, energy, NULL);
    for (int k = 0; k < TSIM_SF_COUNT; k++) {
        if (summed_shortfall(phy, f, dbm, since, energy, k) > 0) {
            return false;
        }
    }
    return true;
}

/* The frame that cost a reception lost on interference the most, or 0 for none, by the rule that
 * judged it: under pairwise, the counted frame furthest past its threshold; summed, the loudest
 * frame of the SF group furthest past its own. */
static uint64_t loudest_rival(const struct tsim_phy *phy, const struct tsim_frame *f, uint32_t node,
                              double dbm, tsim_time since) {
    double most = 0;
    uint64_t id = 0;
    if (phy->params.pairwise) {
        for (size_t i = 0; i < phy->count; i++) {
            const struct tsim_frame *g = &phy->air[phy->head + i].f;
            double short_by = pairwise_shortfall(phy, f, g, node, dbm, since);
            if (short_by > most) {
                most = short_by;
                id = g->id;
            }
        }
        return id;
    }
    double energy[TSIM_SF_COUNT] = {0};
    uint64_t loudest[TSIM_SF_COUNT] = {0};
    interference(phy, f, node, since, energy, loudest);
    for (int k = 0; k < TSIM_SF_COUNT; k++) {
        double short_by = summed_shortfall(phy, f, dbm, since, energy, k);
        if (short_by > most) {
            most = short_by;
            id = loudest[k];
        }
    }
    return id;
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
    uint32_t watch = a->watch;
    uint8_t fate = a->fate;
    uint64_t rival = a->rival;
    size_t done = 0;
    for (uint32_t i = 0; i < phy->n; i++) {
        struct node *nd = &phy->nodes[i];
        if (nd->state != RECEIVE || nd->frame != f.id) {
            continue;
        }
        bool ok = survives(phy, &f, i, nd->frame_dbm, nd->since);
        count_reception(nd);
        if (ok) {
            nd->stats.rx_ok++;
        } else {
            nd->stats.rx_lost++;
        }
        phy->outcomes[done++] = (struct outcome){i, nd->frame_dbm, ok};
        if (i == watch) {
            fate = ok ? TSIM_PHY_DECODED : TSIM_PHY_INTERFERED;
            rival = ok ? 0 : loudest_rival(phy, &f, i, nd->frame_dbm, nd->since);
        }
        nd->state = LISTEN;
    }
    int rival_tag = -1;
    if (watch < phy->n) {
        if (fate == PENDING) {
            fate = decodable(phy, &f, rx_dbm(phy, &f, watch)) ? TSIM_PHY_DEAF : TSIM_PHY_WEAK;
        }
        const struct air *r = rival ? kept(phy, rival) : NULL;
        rival_tag = r ? r->tag : -1;
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
    if (phy->hooks.marked && watch < phy->n) {
        phy->hooks.marked(phy->hooks.ctx, watch, &f, (enum tsim_phy_fate)fate, rival_tag);
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
    if (nodes == 0 || !tsim_lora_valid(listen_on) ||
        !(params->fading_db >= 0 && params->fading_db <= 1e3) ||
        !(params->cad_margin_db >= -1e3 && params->cad_margin_db <= 1e3) || params->cad_delay < 0) {
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
            .header_set = -1,
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
        free(nd->unsettled);
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

void tsim_phy_set_loss_from(struct tsim_phy *phy, uint32_t from, uint32_t to, double loss_db) {
    phy->loss[(size_t)from * phy->n + to] = loss_db;
}

double tsim_phy_loss(const struct tsim_phy *phy, uint32_t a, uint32_t b) {
    return phy->loss[(size_t)a * phy->n + b];
}

static uint32_t root(uint32_t *up, uint32_t x) {
    while (up[x] != x) {
        x = up[x] = up[up[x]];
    }
    return x;
}

uint32_t tsim_phy_nodes(const struct tsim_phy *phy) { return phy->n; }

double tsim_phy_floor_dbm(const struct tsim_phy *phy, const struct tsim_lora *lora) {
    if (!tsim_lora_valid(lora) || lora->sf < TSIM_SF_MIN ||
        lora->sf >= TSIM_SF_MIN + TSIM_SF_COUNT) {
        return NAN;
    }
    return noise_dbm(phy, lora->bw_hz) + phy->params.snr_min_db[lora->sf - TSIM_SF_MIN];
}

bool tsim_phy_links(const struct tsim_phy *phy, const struct tsim_lora *lora, double tx_dbm,
                    struct tsim_phy_links *out) {
    *out = (struct tsim_phy_links){0};
    if (!tsim_lora_valid(lora) || lora->sf < TSIM_SF_MIN ||
        lora->sf >= TSIM_SF_MIN + TSIM_SF_COUNT) {
        return false;
    }
    uint32_t n = phy->n;
    uint32_t *up = malloc(n * sizeof *up);
    uint32_t *degree = calloc(n, sizeof *degree);
    if (!up || !degree) {
        free(up);
        free(degree);
        return false;
    }
    /* Received at least this far below the sender: the floor, as decodable() has it. */
    double budget = tx_dbm - tsim_phy_floor_dbm(phy, lora);
    for (uint32_t a = 0; a < n; a++) {
        up[a] = a;
    }
    uint64_t links = 0;
    for (uint32_t a = 0; a < n; a++) {
        for (uint32_t b = a + 1; b < n; b++) {
            if (phy->loss[(size_t)a * n + b] <= budget && phy->loss[(size_t)b * n + a] <= budget) {
                degree[a]++;
                degree[b]++;
                links++;
                up[root(up, a)] = root(up, b);
            }
        }
    }
    uint32_t *size = calloc(n, sizeof *size);
    if (!size) {
        free(up);
        free(degree);
        return false;
    }
    out->degree_min = n ? UINT32_MAX : 0;
    for (uint32_t a = 0; a < n; a++) {
        uint32_t c = ++size[root(up, a)];
        if (c > out->component_max) {
            out->component_max = c;
        }
        if (degree[a] < out->degree_min) {
            out->degree_min = degree[a];
        }
        if (degree[a] > out->degree_max) {
            out->degree_max = degree[a];
        }
    }
    out->degree_mean = n ? 2.0 * (double)links / n : 0;
    free(size);
    free(up);
    free(degree);
    return true;
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
    if (node >= phy->n || phy->nodes[node].state == TRANSMIT || phy->nodes[node].off) {
        return 0;
    }
    tsim_time airtime = tsim_lora_airtime(lora, len);
    tsim_time now = tsim_sched_now(phy->sched);
    if (airtime < 0 || airtime > INT64_MAX - now) {
        return 0;
    }
    /* Everything that can fail goes first, so a refused frame leaves nothing behind. */
    struct node *tx = &phy->nodes[node];
    struct tsim_event end = tsim_sched_after(phy->sched, airtime, frame_end, tx);
    if (end.slot == 0) {
        return 0;
    }
    struct air *a = push(phy);
    if (!a) {
        tsim_sched_cancel(phy->sched, end);
        return 0;
    }
    a->on_air = true;
    a->tag = -1;
    a->watch = UINT32_MAX;
    a->fate = PENDING;
    a->rival = 0;
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

    if (tx->state == RECEIVE) {
        count_reception(tx);
        settle(tx, tx->frame, lock_by(phy, &find(phy, tx->frame)->f), &tx->stats.rx_aborted,
               TSIM_PHY_DEAF, f->id);
    } else if (tx->state == RETUNE) {
        tsim_sched_cancel(phy->sched, tx->wake);
    }
    tx->state = TRANSMIT;
    tx->frame = f->id;
    tx->wake = end;
    tx->stats.tx++;
    tx->stats.tx_airtime += airtime;

    /* decodable(), with what it works out the same for every receiver worked out once. */
    double noise = noise_dbm(phy, f->lora.bw_hz);
    double snr_min = phy->params.snr_min_db[f->lora.sf - TSIM_SF_MIN];
    tsim_time f_lock_by = lock_by(phy, f);
    uint32_t receivers = now <= f_lock_by ? phy->n : 0;
    for (uint32_t i = 0; i < receivers; i++) {
        struct node *nd = &phy->nodes[i];
        if (i == node || !tuned_to(nd, f)) {
            continue;
        }
        if (nd->state == LISTEN) {
            double dbm = rx_dbm(phy, f, i);
            if (dbm - noise >= snr_min) {
                sync_to(nd, f, dbm);
            }
        } else if (nd->state == RECEIVE) {
            double dbm = rx_dbm(phy, f, i);
            if (!(dbm - noise >= snr_min)) {
                continue;
            }
            if ((phy->params.capture_anytime || now < nd->lock_at) &&
                dbm >= nd->frame_dbm + phy->params.capture_db) {
                count_reception(nd);
                settle(nd, nd->frame, lock_by(phy, &find(phy, nd->frame)->f),
                       &nd->stats.rx_preempted, TSIM_PHY_TAKEN, f->id);
                sync_to(nd, f, dbm);
            } else {
                settle(nd, f->id, f_lock_by, &nd->stats.rx_missed, TSIM_PHY_BUSY, nd->frame);
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
        count_reception(nd);
        settle(nd, nd->frame, lock_by(phy, &find(phy, nd->frame)->f), &nd->stats.rx_aborted,
               TSIM_PHY_DEAF, 0);
    }
    begin_retune(nd);
    return true;
}

void tsim_phy_power(struct tsim_phy *phy, uint32_t node, bool on) {
    struct node *nd = &phy->nodes[node];
    if (nd->off != on) {
        return;
    }
    nd->off = !on;
    if (on) {
        if (nd->state == LISTEN) {
            listen(nd);
        }
        return;
    }
    /* What it was receiving is lost; a frame it is sending, or a retune, runs to its end. */
    if (nd->state == RECEIVE) {
        count_reception(nd);
        settle(nd, nd->frame, lock_by(phy, &find(phy, nd->frame)->f), &nd->stats.rx_aborted,
               TSIM_PHY_DEAF, 0);
        nd->state = LISTEN;
    }
}

bool tsim_phy_on(const struct tsim_phy *phy, uint32_t node) { return !phy->nodes[node].off; }

bool tsim_phy_mark(struct tsim_phy *phy, uint64_t frame, uint32_t node, int tag) {
    struct air *a = kept(phy, frame);
    if (!a || !a->on_air || tag < 0 || tag > 127) {
        return false;
    }
    a->tag = (int8_t)tag;
    if (node >= phy->n || node == a->f.src) {
        return true;
    }
    /* Where the receiver stands now, as the frame begins; what it does later rebooks it. */
    const struct node *nd = &phy->nodes[node];
    a->watch = node;
    a->rival = 0;
    if (nd->state == RECEIVE && nd->frame == frame) {
        a->fate = PENDING;
    } else if (nd->state == TRANSMIT || nd->state == RETUNE || !tuned_to(nd, &a->f)) {
        a->fate = TSIM_PHY_DEAF;
        a->rival = nd->state == TRANSMIT ? nd->frame : 0;
    } else if (!decodable(phy, &a->f, rx_dbm(phy, &a->f, node))) {
        a->fate = TSIM_PHY_WEAK;
    } else if (nd->state == RECEIVE) {
        a->fate = TSIM_PHY_BUSY;
        a->rival = nd->frame;
    } else {
        a->fate = TSIM_PHY_DEAF; /* listening, but too late in the preamble to lock on */
    }
    return true;
}

bool tsim_phy_transmitting(const struct tsim_phy *phy, uint32_t node) {
    return phy->nodes[node].state == TRANSMIT;
}

bool tsim_phy_receiving(const struct tsim_phy *phy, uint32_t node) {
    return phy->nodes[node].state == RECEIVE;
}

void tsim_phy_hold_header(struct tsim_phy *phy, uint32_t node, tsim_time hold) {
    struct node *nd = &phy->nodes[node];
    nd->header_hold = hold > 0 ? hold : 0;
    nd->header_set = -1;
}

bool tsim_phy_carrier(struct tsim_phy *phy, uint32_t node) {
    struct node *nd = &phy->nodes[node];
    if (nd->off) {
        return false;
    }
    if (nd->header_hold == 0) {
        return nd->state == RECEIVE;
    }
    tsim_time now = tsim_sched_now(phy->sched);
    if (nd->header_set < 0) {
        return false; /* anything it is on began before the flags were last cleared */
    }
    if (now < nd->header_set) {
        return true; /* the preamble of the reception that will set it */
    }
    if (now - nd->header_set > nd->header_hold) {
        nd->header_set = -1;
        return false;
    }
    return true;
}

bool tsim_phy_cad(const struct tsim_phy *phy, uint32_t node) {
    const struct node *nd = &phy->nodes[node];
    if (nd->state == TRANSMIT || nd->state == RETUNE) {
        return false;
    }
    tsim_time now = tsim_sched_now(phy->sched);
    for (size_t i = 0; i < phy->count; i++) {
        const struct air *a = &phy->air[phy->head + i];
        if (a->on_air && a->f.src != node && tuned_to(nd, &a->f) &&
            now - a->f.start >= phy->params.cad_delay &&
            decodable(phy, &a->f, rx_dbm(phy, &a->f, node) + phy->params.cad_margin_db)) {
            return true;
        }
    }
    return false;
}

const struct tsim_phy_stats *tsim_phy_stats(const struct tsim_phy *phy, uint32_t node) {
    return &phy->nodes[node].stats;
}

tsim_time tsim_phy_rx_airtime(const struct tsim_phy *phy, uint32_t node) {
    const struct node *nd = &phy->nodes[node];
    tsim_time t = nd->stats.rx_airtime;
    if (nd->state == RECEIVE) {
        t += tsim_sched_now(phy->sched) - nd->since;
    }
    return t;
}
