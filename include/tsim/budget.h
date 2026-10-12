#ifndef TSIM_BUDGET_H
#define TSIM_BUDGET_H

#include <stdbool.h>
#include <stdint.h>

#include "tsim/time.h"

/* Tern's airtime budget, as the specification's draft/airtime.md has it, and its reference model
 * vectors/tools/airtime.py: what a node may spend on the frames it writes and on those it passes
 * on, each from a token bucket of its own, and what becomes of a frame its bucket does not hold.
 *
 *  - own: the frames a node writes - a message, an acknowledgement, a flood it begins. Filled at
 *    `own_ppm` millionths of its time, holding `own_window` of that. A frame of its own its buckets
 *    do not hold waits until they do, and those behind it wait behind it.
 *  - relayed: every other frame it sends but routing's. Filled at `relayed_ppm`, holding
 *    `relayed_window`. One its buckets do not hold when it would be offered to the radio, after
 *    whatever wait its MAC drew, is dropped.
 *  - an acknowledgement, of either part, is never held or dropped: it is charged to its part, and
 *    the bucket goes no lower than empty.
 *  - with `floods`, the two allowances of draft/flooding.md stay inside the parts: a flooded frame
 *    of the node's own is paid for by the flood bucket for its own floods as well as the own part,
 *    and one passed on by the flood bucket for those passed on as well as the relayed part.
 *
 * Announces and requests are no part of it: the routing's own cap pays for those, as it did.
 * Every bucket starts full, holds its window of its share or one 255-byte frame if that is more,
 * and is kept in millionths of a nanosecond of airtime, so that nothing is rounded but a wait.
 *
 * The network keeps one for each node (tsim/net.h): a routing that wants its frames paid for marks
 * each with the part that pays (tsim_tx.budget), and one that marks none - every candidate but
 * Tern's - is not touched by it. The region's duty cycle (net.duty_cycle) stays a check of its
 * own, beside the budget, and a frame goes only once both allow it. */

/* What a routing marks a frame with, in tsim_tx.budget: 0 for one no part pays for. */
#define TSIM_BUDGET_OWN 0x01     /* a frame the node wrote: waits until paid for */
#define TSIM_BUDGET_RELAYED 0x02 /* a frame it passes on: dropped if not paid for when due */
#define TSIM_BUDGET_ACK 0x04     /* with either, an acknowledgement: never held, never dropped */
#define TSIM_BUDGET_FLOOD 0x08   /* with either, a flooded frame: a flood allowance pays too */

enum tsim_budget_mode {
    TSIM_BUDGET_MODE_OFF,
    TSIM_BUDGET_MODE_ON,
    /* Kept but not enforced: every frame goes as with none, is charged as a frame sent, and what
     * the budget would have held or dropped is counted - what binds, without what binding does to
     * the mesh. */
    TSIM_BUDGET_MODE_SHADOW,
};

struct tsim_budget_params {
    enum tsim_budget_mode mode;
    uint32_t own_ppm; /* millionths of the node's time, 1 to 1000000 */
    tsim_time own_window;
    uint32_t relayed_ppm;
    tsim_time relayed_window;
    bool floods; /* the flood allowances, inside the parts */
    uint32_t flood_own_ppm;
    tsim_time flood_own_window;
    uint32_t flood_relayed_ppm;
    tsim_time flood_relayed_window;
};

/* Off, with the specification's values for when it is on: own 2% of 600 s, relayed 7% of 60 s,
 * and the flood allowances, 0.5% of 600 s and 3% of 60 s. */
struct tsim_budget_params tsim_budget_defaults(void);

/* Whether the parameters are ones a bucket can keep: shares from 1 to 1000000 ppm and windows from
 * 1 ns to an hour, so that no level overflows. */
bool tsim_budget_valid(const struct tsim_budget_params *params);

/* A token bucket, in millionths of a nanosecond of airtime. */
struct tsim_bucket {
    int64_t ppm;
    int64_t room;
    int64_t level;
    tsim_time at; /* when `level` was */
};

/* Full at `at`: `window` of `ppm`, or `frame` (the airtime of one 255-byte frame) if that is more.
 */
void tsim_bucket_init(struct tsim_bucket *bucket, uint32_t ppm, tsim_time window, tsim_time frame,
                      tsim_time at);

/* What it holds at `at`, which is no earlier than the last charge. */
int64_t tsim_bucket_level(const struct tsim_bucket *bucket, tsim_time at);

bool tsim_bucket_holds(const struct tsim_bucket *bucket, tsim_time at, tsim_time airtime);

/* Takes a frame's airtime at `at`, going no lower than empty. */
void tsim_bucket_charge(struct tsim_bucket *bucket, tsim_time at, tsim_time airtime);

/* How long from `at` until it holds `airtime`: rounded up to the nanosecond, 0 if it does now. */
tsim_time tsim_bucket_wait(const struct tsim_bucket *bucket, tsim_time at, tsim_time airtime);

enum tsim_budget_bucket {
    TSIM_BUCKET_OWN,
    TSIM_BUCKET_RELAYED,
    TSIM_BUCKET_FLOOD_OWN,
    TSIM_BUCKET_FLOOD_RELAYED,
    TSIM_BUCKET_COUNT,
};

/* One node's buckets. A leaf has a relayed part it never spends: nothing it sends is passed on. */
struct tsim_budget {
    struct tsim_bucket bucket[TSIM_BUCKET_COUNT];
    bool floods;
};

/* Every bucket full at `at`. */
void tsim_budget_init(struct tsim_budget *budget, const struct tsim_budget_params *params,
                      tsim_time frame, tsim_time at);

/* The buckets that pay for a frame marked `mark`, one bit for each enum tsim_budget_bucket; 0 for
 * one no part pays for. */
unsigned tsim_budget_payers(const struct tsim_budget *budget, uint8_t mark);

/* Whether every bucket that pays for it could hold it at all, full: a frame one of them could not
 * would never be paid for. */
bool tsim_budget_fits(const struct tsim_budget *budget, uint8_t mark, tsim_time airtime);

/* Whether every bucket that pays for it holds it at `at`. A frame no part pays for always is. */
bool tsim_budget_paid(const struct tsim_budget *budget, tsim_time at, uint8_t mark,
                      tsim_time airtime);

/* Charges a frame sent at `at` to every bucket that pays for it, all at once. */
void tsim_budget_charge(struct tsim_budget *budget, tsim_time at, uint8_t mark, tsim_time airtime);

/* How long from `at` until every bucket that pays for it holds it, if nothing else is charged. */
tsim_time tsim_budget_wait(const struct tsim_budget *budget, tsim_time at, uint8_t mark,
                           tsim_time airtime);

#endif
