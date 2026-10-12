#include "tsim/budget.h"

#define PPM 1000000
#define WINDOW_MAX TSIM_S(3600)

struct tsim_budget_params tsim_budget_defaults(void) {
    return (struct tsim_budget_params){
        .mode = TSIM_BUDGET_MODE_OFF,
        .own_ppm = 10000,
        .own_window = TSIM_S(600),
        .relayed_ppm = 80000,
        .relayed_window = TSIM_S(60),
        .floods = true,
        .flood_own_ppm = 5000,
        .flood_own_window = TSIM_S(600),
        .flood_relayed_ppm = 30000,
        .flood_relayed_window = TSIM_S(60),
    };
}

static bool share_ok(uint32_t ppm, tsim_time window) {
    return ppm >= 1 && ppm <= PPM && window >= 1 && window <= WINDOW_MAX;
}

bool tsim_budget_valid(const struct tsim_budget_params *p) {
    return (int)p->mode >= TSIM_BUDGET_MODE_OFF && p->mode <= TSIM_BUDGET_MODE_SHADOW &&
           share_ok(p->own_ppm, p->own_window) && share_ok(p->relayed_ppm, p->relayed_window) &&
           share_ok(p->flood_own_ppm, p->flood_own_window) &&
           share_ok(p->flood_relayed_ppm, p->flood_relayed_window);
}

/* --- One bucket --- */

void tsim_bucket_init(struct tsim_bucket *b, uint32_t ppm, tsim_time window, tsim_time frame,
                      tsim_time at) {
    /* An hour of a whole share is 3.6e18, and a frame of 255 bytes at SF12 14e15: both fit. */
    int64_t room = (int64_t)ppm * window, one = frame * PPM;
    b->ppm = ppm;
    b->room = room > one ? room : one;
    b->level = b->room;
    b->at = at;
}

int64_t tsim_bucket_level(const struct tsim_bucket *b, tsim_time at) {
    int64_t gap = b->room - b->level;
    tsim_time dt = at - b->at;
    /* Full once it has had longer than the gap takes to fill; until then dt * ppm is no more than
     * the gap, so the product never overflows however long the bucket went untouched. */
    if (dt <= 0) {
        return b->level;
    }
    return dt > gap / b->ppm ? b->room : b->level + dt * b->ppm;
}

bool tsim_bucket_holds(const struct tsim_bucket *b, tsim_time at, tsim_time airtime) {
    return tsim_bucket_level(b, at) >= airtime * PPM;
}

void tsim_bucket_charge(struct tsim_bucket *b, tsim_time at, tsim_time airtime) {
    int64_t level = tsim_bucket_level(b, at) - airtime * PPM;
    b->level = level > 0 ? level : 0;
    b->at = at > b->at ? at : b->at;
}

tsim_time tsim_bucket_wait(const struct tsim_bucket *b, tsim_time at, tsim_time airtime) {
    int64_t is_short = airtime * PPM - tsim_bucket_level(b, at);
    return is_short <= 0 ? 0 : (is_short + b->ppm - 1) / b->ppm;
}

/* --- A node's buckets --- */

void tsim_budget_init(struct tsim_budget *bd, const struct tsim_budget_params *p, tsim_time frame,
                      tsim_time at) {
    tsim_bucket_init(&bd->bucket[TSIM_BUCKET_OWN], p->own_ppm, p->own_window, frame, at);
    tsim_bucket_init(&bd->bucket[TSIM_BUCKET_RELAYED], p->relayed_ppm, p->relayed_window, frame,
                     at);
    tsim_bucket_init(&bd->bucket[TSIM_BUCKET_FLOOD_OWN], p->flood_own_ppm, p->flood_own_window,
                     frame, at);
    tsim_bucket_init(&bd->bucket[TSIM_BUCKET_FLOOD_RELAYED], p->flood_relayed_ppm,
                     p->flood_relayed_window, frame, at);
    bd->floods = p->floods;
}

unsigned tsim_budget_payers(const struct tsim_budget *bd, uint8_t mark) {
    bool own = mark & TSIM_BUDGET_OWN, relayed = mark & TSIM_BUDGET_RELAYED;
    if (own == relayed) {
        return 0; /* neither, or a mark no frame can have */
    }
    unsigned payers = 1u << (own ? TSIM_BUCKET_OWN : TSIM_BUCKET_RELAYED);
    if (bd->floods && (mark & TSIM_BUDGET_FLOOD)) {
        payers |= 1u << (own ? TSIM_BUCKET_FLOOD_OWN : TSIM_BUCKET_FLOOD_RELAYED);
    }
    return payers;
}

bool tsim_budget_fits(const struct tsim_budget *bd, uint8_t mark, tsim_time airtime) {
    unsigned payers = tsim_budget_payers(bd, mark);
    for (int i = 0; i < TSIM_BUCKET_COUNT; i++) {
        if ((payers >> i & 1) && bd->bucket[i].room < airtime * PPM) {
            return false;
        }
    }
    return true;
}

bool tsim_budget_paid(const struct tsim_budget *bd, tsim_time at, uint8_t mark, tsim_time airtime) {
    unsigned payers = tsim_budget_payers(bd, mark);
    for (int i = 0; i < TSIM_BUCKET_COUNT; i++) {
        if ((payers >> i & 1) && !tsim_bucket_holds(&bd->bucket[i], at, airtime)) {
            return false;
        }
    }
    return true;
}

void tsim_budget_charge(struct tsim_budget *bd, tsim_time at, uint8_t mark, tsim_time airtime) {
    unsigned payers = tsim_budget_payers(bd, mark);
    for (int i = 0; i < TSIM_BUCKET_COUNT; i++) {
        if (payers >> i & 1) {
            tsim_bucket_charge(&bd->bucket[i], at, airtime);
        }
    }
}

tsim_time tsim_budget_paid_since(const struct tsim_budget *bd, tsim_time at, uint8_t mark,
                                 tsim_time airtime) {
    if (!tsim_budget_paid(bd, at, mark, airtime)) {
        return -1;
    }
    unsigned payers = tsim_budget_payers(bd, mark);
    tsim_time since = INT64_MIN;
    for (int i = 0; i < TSIM_BUCKET_COUNT; i++) {
        if (payers >> i & 1) {
            const struct tsim_bucket *b = &bd->bucket[i];
            tsim_time t = b->at + tsim_bucket_wait(b, b->at, airtime);
            since = t > since ? t : since;
        }
    }
    return since;
}

tsim_time tsim_budget_wait(const struct tsim_budget *bd, tsim_time at, uint8_t mark,
                           tsim_time airtime) {
    unsigned payers = tsim_budget_payers(bd, mark);
    tsim_time wait = 0;
    for (int i = 0; i < TSIM_BUCKET_COUNT; i++) {
        if (payers >> i & 1) {
            tsim_time w = tsim_bucket_wait(&bd->bucket[i], at, airtime);
            wait = w > wait ? w : wait;
        }
    }
    return wait;
}
