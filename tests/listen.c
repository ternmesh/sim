#include "tsim/listen.h"

#include "tsim/baseline.h"
#include "tsim/net.h"

#include "check.h"

enum { NODES = 3, LEN = 40 };

static const tsim_time DETECT = TSIM_MS(6), TURNAROUND = TSIM_MS(1), SLOT = TSIM_MS(7);

struct rig {
    struct tsim_sched *sched;
    struct tsim_net *net;
    struct tsim_lora lora;
    tsim_time started[NODES]; /* when each node's last frame began, or -1 */
    uint32_t count[NODES];
};

struct send {
    struct rig *rig;
    uint32_t node;
};

static void heard(void *ctx, const struct tsim_net_heard *h) {
    struct rig *r = ctx;
    if (r->started[h->from] != h->start) {
        r->started[h->from] = h->start;
        r->count[h->from]++;
    }
}

/* Three nodes; `hears` says whether they hear one another. The test stands in for routing and
 * drives the queues directly. */
static void rig_open(struct rig *r, uint32_t window, bool hears, uint64_t seed) {
    struct tsim_net_params p = tsim_net_defaults(seed);
    struct tsim_flood_config fc = {.lora = tsim_lora_default(9, 500000), .tx_dbm = 14.0, .hops = 0};
    struct tsim_listen_config lc = {
        .detect = DETECT, .turnaround = TURNAROUND, .slot = SLOT, .window = window};
    p.listen = fc.lora;
    *r = (struct rig){.sched = tsim_sched_create(), .lora = fc.lora};
    r->net = tsim_net_create(r->sched, &p, NODES, &tsim_flood, &fc, &tsim_listen, &lc);
    for (uint32_t i = 0; i < NODES; i++) {
        r->started[i] = -1;
        for (uint32_t j = i + 1; hears && j < NODES; j++) {
            tsim_phy_set_loss(tsim_net_phy(r->net), i, j, 100.0);
        }
    }
    static const bool all[NODES] = {true, true, true};
    tsim_net_observe_heard(r->net, heard, r, all);
    tsim_net_start(r->net);
}

static void rig_close(struct rig *r) {
    tsim_net_destroy(r->net);
    tsim_sched_destroy(r->sched);
}

static void send(struct tsim_sched *sched, void *ctx) {
    (void)sched;
    struct send *s = ctx;
    struct tsim_tx tx = {
        .lora = s->rig->lora, .tx_dbm = 14.0, .purpose = TSIM_PURPOSE_DATA, .len = LEN};
    tsim_node_send(tsim_net_node(s->rig->net, s->node), &tx);
}

static tsim_time airtime(const struct rig *r) { return tsim_lora_airtime(&r->lora, LEN); }

/* Node 0 is given a frame at 0, and so is on the air from TURNAROUND; node 1 is given one `after`
 * into that frame. Returns when node 1's began. */
static tsim_time second_starts(struct rig *r, tsim_time after) {
    struct send first = {r, 0}, second = {r, 1};
    tsim_sched_at(r->sched, 0, send, &first);
    tsim_sched_at(r->sched, TURNAROUND + after, send, &second);
    tsim_sched_run_until(r->sched, TSIM_S(10));
    CHECK_EQ_I64(r->started[0], TURNAROUND);
    CHECK_EQ_U64(r->count[1], 1);
    return r->started[1];
}

static void a_node_waits_out_the_frame_it_is_receiving(void) {
    struct rig r;
    rig_open(&r, 0, true, 1);
    tsim_time began = second_starts(&r, DETECT);
    tsim_time end = TURNAROUND + airtime(&r);
    /* As the frame ends, give or take the instant it looks again, and no slots after. */
    CHECK(began >= end + TURNAROUND && began <= end + TURNAROUND + 1);
    rig_close(&r);
}

/* What a radio cannot do: the frame has not been on the air long enough to be noticed. */
static void a_frame_that_has_just_begun_is_not_noticed(void) {
    struct rig r;
    rig_open(&r, 0, true, 1);
    tsim_time began = second_starts(&r, DETECT - 1);
    CHECK_EQ_I64(began, TURNAROUND + DETECT - 1 + TURNAROUND);
    rig_close(&r);
}

static void a_frame_it_cannot_hear_does_not_hold_it(void) {
    struct rig r;
    rig_open(&r, 0, false, 1);
    tsim_time began = second_starts(&r, 4 * DETECT);
    CHECK_EQ_I64(began, TURNAROUND + 4 * DETECT + TURNAROUND);
    rig_close(&r);
}

/* It looked, found nothing, and is turning round: a frame that begins then does not stop it. */
static void it_does_not_look_again_once_it_has_decided(void) {
    struct rig r;
    rig_open(&r, 0, true, 1);
    struct send first = {&r, 0}, second = {&r, 1};
    tsim_sched_at(r.sched, 0, send, &first);
    tsim_sched_at(r.sched, 0, send, &second);
    tsim_sched_run_until(r.sched, TSIM_S(10));
    CHECK_EQ_I64(r.started[0], TURNAROUND);
    CHECK_EQ_I64(r.started[1], TURNAROUND);
    rig_close(&r);
}

static void a_window_adds_whole_slots_after_the_frame(void) {
    enum { WINDOW = 4, SEEDS = 40 };
    bool seen[WINDOW + 1] = {false};
    for (uint64_t seed = 1; seed <= SEEDS; seed++) {
        struct rig r;
        rig_open(&r, WINDOW, true, seed);
        tsim_time over = second_starts(&r, DETECT) - (TURNAROUND + airtime(&r)) - TURNAROUND;
        CHECK(over >= 0 && over / SLOT <= WINDOW && over % SLOT <= 1);
        if (over >= 0 && over / SLOT <= WINDOW) {
            seen[over / SLOT] = true;
        }
        rig_close(&r);
    }
    for (int k = 0; k <= WINDOW; k++) {
        CHECK(seen[k]);
    }
}

/* Two nodes that waited for the same frame start together when it ends, with no window, and with
 * one may not. */
static tsim_time apart_after_one_frame(uint32_t window, uint64_t seed) {
    struct rig r;
    rig_open(&r, window, true, seed);
    struct send first = {&r, 0}, second = {&r, 1}, third = {&r, 2};
    tsim_sched_at(r.sched, 0, send, &first);
    tsim_sched_at(r.sched, TURNAROUND + DETECT, send, &second);
    tsim_sched_at(r.sched, TURNAROUND + 2 * DETECT, send, &third);
    tsim_sched_run_until(r.sched, TSIM_S(10));
    CHECK_EQ_U64(r.count[1], 1);
    CHECK_EQ_U64(r.count[2], 1);
    tsim_time apart = r.started[2] - r.started[1];
    rig_close(&r);
    return apart < 0 ? -apart : apart;
}

static void two_that_waited_for_one_frame_start_together(void) {
    CHECK(apart_after_one_frame(0, 1) <= 1);
    uint32_t apart = 0;
    for (uint64_t seed = 1; seed <= 20; seed++) {
        apart += apart_after_one_frame(8, seed) >= SLOT;
    }
    CHECK(apart > 10);
}

int main(void) {
    RUN(a_node_waits_out_the_frame_it_is_receiving);
    RUN(a_frame_that_has_just_begun_is_not_noticed);
    RUN(a_frame_it_cannot_hear_does_not_hold_it);
    RUN(it_does_not_look_again_once_it_has_decided);
    RUN(a_window_adds_whole_slots_after_the_frame);
    RUN(two_that_waited_for_one_frame_start_together);
    return CHECK_DONE();
}
