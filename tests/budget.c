#include "tsim/budget.h"

#include <stdbool.h>
#include <string.h>

#include "check.h"
#include "tsim/lora.h"

/* The airtime budget against the specification's vectors (ternmesh/spec, vectors/airtime.json,
 * from vectors/tools/airtime.py at the commit that merged draft/airtime.md): the spends and the
 * estimates, frame by frame, and the reference's own hand-worked checks. */

/* The two profiles the vectors use, as Tern's frames go: a 16-symbol preamble, coding rate 4/5. */
static struct tsim_lora profile(bool us915) {
    struct tsim_lora m = tsim_lora_default(us915 ? 9 : 7, us915 ? 500000 : 125000);
    m.preamble = 16;
    return m;
}

/* What the reference's part() and flood_bucket() say pays for a frame with this hdr. */
static uint8_t mark_of(uint8_t hdr, bool own) {
    uint8_t mark = own ? TSIM_BUDGET_OWN : TSIM_BUDGET_RELAYED;
    if (hdr == 0x59 || hdr == 0x5A) {
        return 0;
    }
    if (hdr == 0x50) {
        mark |= TSIM_BUDGET_ACK;
    }
    if (hdr == 0x60 || hdr == 0x61 || hdr == 0x68) {
        mark |= TSIM_BUDGET_FLOOD;
    }
    return mark;
}

struct node {
    struct tsim_budget budget;
    struct tsim_lora lora;
};

static void node_init(struct node *n, bool us915) {
    struct tsim_budget_params p = tsim_budget_defaults();
    p.mode = TSIM_BUDGET_MODE_ON;
    n->lora = profile(us915);
    tsim_budget_init(&n->budget, &p, tsim_lora_airtime(&n->lora, 255), 0);
}

/* As the reference's Node.offer(): sent if every bucket that pays holds it, or it is an
 * acknowledgement, and then charged to all of them. */
static bool offer(struct node *n, tsim_time at, uint8_t hdr, bool own, uint32_t len) {
    uint8_t mark = mark_of(hdr, own);
    tsim_time air = tsim_lora_airtime(&n->lora, len);
    bool sent = (mark & TSIM_BUDGET_ACK) || tsim_budget_paid(&n->budget, at, mark, air);
    if (sent) {
        tsim_budget_charge(&n->budget, at, mark, air);
    }
    return sent;
}

struct offer {
    tsim_time at;
    uint8_t hdr;
    bool own;
    uint32_t len;
    bool sent;
};

static void spend(bool us915, const struct offer *o, size_t count) {
    struct node n;
    node_init(&n, us915);
    for (size_t i = 0; i < count; i++) {
        if (offer(&n, o[i].at, o[i].hdr, o[i].own, o[i].len) != o[i].sent) {
            fprintf(stderr, "  frame %zu at %" PRId64 " ns: sent should be %d\n", i, o[i].at,
                    o[i].sent);
            check_failures++;
        }
    }
}

static void airtimes_are_the_vectors(void) {
    struct tsim_lora eu = profile(false), us = profile(true);
    CHECK_EQ_I64(tsim_lora_airtime(&eu, 255), 407808000);
    CHECK_EQ_I64(tsim_lora_airtime(&eu, 67), 131328000);
    CHECK_EQ_I64(tsim_lora_airtime(&eu, 19), 59648000);
    CHECK_EQ_I64(tsim_lora_airtime(&us, 255), 320768000);
    CHECK_EQ_I64(tsim_lora_airtime(&us, 19), 54528000);
    CHECK_EQ_I64(tsim_lora_airtime(&us, 56), 95488000);
}

/* Spends "own": a leaf's own part, emptied by a burst, and filling again. */
static void own_part_empties_and_fills(void) {
    struct offer o[19];
    for (int i = 0; i < 15; i++) {
        o[i] = (struct offer){0, 0x48, true, 255, i < 14};
    }
    o[15] = (struct offer){11711999999, 0x48, true, 255, false};
    o[16] = (struct offer){11712000000, 0x48, true, 255, true};
    o[17] = (struct offer){11712000000, 0x52, true, 60, false};
    o[18] = (struct offer){TSIM_S(600), 0x48, true, 67, true};
    spend(false, o, 19);
}

/* Spends "acknowledgements": an acknowledgement goes when the part is empty, and leaves it empty,
 * not owing. */
static void acknowledgements_go_and_leave_no_debt(void) {
    struct offer o[23];
    for (int i = 0; i < 14; i++) {
        o[i] = (struct offer){0, 0x48, true, 255, true};
    }
    for (int i = 14; i < 20; i++) {
        o[i] = (struct offer){0, 0x50, true, 19, true};
    }
    o[20] = (struct offer){0, 0x48, true, 23, false};
    o[21] = (struct offer){40780799999, 0x48, true, 255, false};
    o[22] = (struct offer){40780800000, 0x48, true, 255, true};
    spend(false, o, 23);
}

/* Spends "own-floods": the own flood allowance binds first, and a message still goes from what the
 * own part has left. */
static void own_floods_bind_first(void) {
    struct offer o[10];
    for (int i = 0; i < 8; i++) {
        o[i] = (struct offer){0, 0x60, true, 255, i < 7};
    }
    o[8] = (struct offer){0, 0x48, true, 255, true};
    o[9] = (struct offer){0, 0x68, true, 134, false};
    spend(false, o, 10);
}

/* Spends "relayed": frames passed on empty the relayed part; the node's own still go. */
static void relayed_part_is_apart(void) {
    struct offer o[19];
    for (int i = 0; i < 15; i++) {
        o[i] = (struct offer){0, 0x48, false, 255, i < 14};
    }
    o[15] = (struct offer){0, 0x48, true, 255, true};
    o[16] = (struct offer){0, 0x50, false, 19, true};
    o[17] = (struct offer){825599999, 0x48, false, 255, false};
    o[18] = (struct offer){825600000, 0x48, false, 255, true};
    spend(true, o, 19);
}

/* Spends "relayed-floods": the relay flood allowance binds first, and other frames passed on
 * still go. */
static void relayed_floods_bind_first(void) {
    struct offer o[9];
    for (int i = 0; i < 7; i++) {
        o[i] = (struct offer){i * TSIM_MS(10), 0x60, false, 255, i < 5};
    }
    o[7] = (struct offer){TSIM_S(1), 0x51, false, 56, true};
    o[8] = (struct offer){TSIM_S(1), 0x61, false, 31, true};
    spend(true, o, 9);
}

/* As the reference's Node.waits_s(): the frames waiting go in order, each when its buckets hold it
 * and not before the one ahead, charged as it goes; in seconds, rounded up. */
static void waits(const struct node *n, tsim_time at, const uint8_t *hdr, const uint32_t *len,
                  size_t count, const int64_t *expected) {
    struct tsim_budget b = n->budget;
    tsim_time t = at;
    for (size_t i = 0; i < count; i++) {
        uint8_t mark = mark_of(hdr[i], true);
        tsim_time air = tsim_lora_airtime(&n->lora, len[i]);
        t += tsim_budget_wait(&b, t, mark, air);
        tsim_budget_charge(&b, t, mark, air);
        int64_t s = (t - at + TSIM_S(1) - 1) / TSIM_S(1);
        CHECK_EQ_I64(s, expected[i]);
    }
}

static void estimates_are_the_vectors(void) {
    struct node n;
    /* "messages" and "later": a leaf that spent fourteen 255-byte frames at 0. */
    node_init(&n, false);
    for (int i = 0; i < 14; i++) {
        offer(&n, 0, 0x48, true, 255);
    }
    const uint8_t m3[] = {0x48, 0x48, 0x48};
    const uint32_t l3[] = {255, 67, 255};
    waits(&n, 0, m3, l3, 3, (const int64_t[]){12, 25, 66});
    waits(&n, TSIM_S(30), m3, l3, 3, (const int64_t[]){0, 0, 36});

    /* "group-behind-message": a group frame waits for the flood bucket behind a message. */
    node_init(&n, false);
    for (int i = 0; i < 7; i++) {
        offer(&n, 0, 0x60, true, 255);
    }
    for (int i = 0; i < 6; i++) {
        offer(&n, 0, 0x48, true, 255);
    }
    waits(&n, 0, (const uint8_t[]){0x48, 0x60, 0x60}, (const uint32_t[]){255, 255, 120}, 3,
          (const int64_t[]){0, 53, 95});

    /* "us915": eighteen frames spent, then four short ones. */
    node_init(&n, true);
    for (int i = 0; i < 18; i++) {
        offer(&n, 0, 0x48, true, 255);
    }
    waits(&n, 0, (const uint8_t[]){0x48, 0x48, 0x48, 0x48}, (const uint32_t[]){23, 23, 23, 23}, 4,
          (const int64_t[]){0, 0, 0, 2});

    /* "message-behind-group": a message the own part could pay for waits as long as the group
     * frame ahead of it. */
    node_init(&n, false);
    for (int i = 0; i < 7; i++) {
        offer(&n, 0, 0x60, true, 255);
    }
    waits(&n, 0, (const uint8_t[]){0x60, 0x48}, (const uint32_t[]){255, 255}, 2,
          (const int64_t[]){53, 53});
}

/* The reference's self_check(), worked by hand there. */
static void hand_worked(void) {
    struct node n;
    node_init(&n, false);
    for (int i = 0; i < 14; i++) {
        CHECK(offer(&n, 0, 0x48, true, 255));
    }
    CHECK(!offer(&n, 0, 0x48, true, 255));
    /* What is left is 290.688 ms; the rest of a frame, 117.12 ms, takes 11.712 s to come. */
    CHECK_EQ_I64(tsim_bucket_level(&n.budget.bucket[TSIM_BUCKET_OWN], 0),
                 (int64_t)290688000 * 1000000);
    CHECK_EQ_I64(tsim_budget_wait(&n.budget, 0, TSIM_BUDGET_OWN, 407808000), 11712000000);
    CHECK(offer(&n, 11712000000, 0x48, true, 255));
    /* Then empty: an acknowledgement still goes, and leaves no debt. */
    CHECK(offer(&n, 11712000000, 0x50, true, 19));
    CHECK_EQ_I64(tsim_bucket_level(&n.budget.bucket[TSIM_BUCKET_OWN], 11712000000), 0);
    CHECK(!offer(&n, 52492799999, 0x48, true, 255));
    CHECK(offer(&n, 52492800000, 0x48, true, 255));
    /* 8% of a minute is 4.8 s: fourteen 255-byte frames on US915. */
    node_init(&n, true);
    for (int i = 0; i < 14; i++) {
        CHECK(offer(&n, 0, 0x48, false, 255));
    }
    CHECK(!offer(&n, 0, 0x48, false, 255));
    CHECK(offer(&n, 0, 0x48, true, 255));
}

/* A bucket holds one 255-byte frame at least, whatever its share and window; it never fills past
 * its room however long it goes untouched; and an announce is no part's. */
static void buckets_hold_a_frame_and_no_more_than_their_room(void) {
    struct tsim_bucket b;
    tsim_bucket_init(&b, 1, TSIM_S(1), 407808000, 0);
    CHECK_EQ_I64(b.room, (int64_t)407808000 * 1000000);
    tsim_bucket_charge(&b, 0, 407808000);
    CHECK_EQ_I64(tsim_bucket_level(&b, TSIM_S(3600) * 24 * 365), b.room);
    tsim_bucket_init(&b, 1000000, TSIM_S(3600), 1, 0);
    tsim_bucket_charge(&b, 0, TSIM_S(3600));
    CHECK_EQ_I64(tsim_bucket_level(&b, INT64_MAX / 2), b.room);
    struct node n;
    node_init(&n, false);
    CHECK(tsim_budget_payers(&n.budget, mark_of(0x59, true)) == 0);
    CHECK(tsim_budget_payers(&n.budget, TSIM_BUDGET_OWN | TSIM_BUDGET_RELAYED) == 0);
    CHECK(tsim_budget_paid(&n.budget, 0, 0, TSIM_S(100)));
    /* Without the flood allowances, a flood is paid for by its part alone. */
    n.budget.floods = false;
    CHECK(tsim_budget_payers(&n.budget, TSIM_BUDGET_OWN | TSIM_BUDGET_FLOOD) ==
          1u << TSIM_BUCKET_OWN);
}

static void params_are_checked(void) {
    struct tsim_budget_params p = tsim_budget_defaults();
    CHECK(p.mode == TSIM_BUDGET_MODE_OFF && tsim_budget_valid(&p));
    CHECK(p.own_ppm == 10000 && p.own_window == TSIM_S(600));
    CHECK(p.relayed_ppm == 80000 && p.relayed_window == TSIM_S(60));
    p.relayed_ppm = 0;
    CHECK(!tsim_budget_valid(&p));
    p = tsim_budget_defaults();
    p.own_window = TSIM_S(3601);
    CHECK(!tsim_budget_valid(&p));
}

int main(void) {
    RUN(airtimes_are_the_vectors);
    RUN(own_part_empties_and_fills);
    RUN(acknowledgements_go_and_leave_no_debt);
    RUN(own_floods_bind_first);
    RUN(relayed_part_is_apart);
    RUN(relayed_floods_bind_first);
    RUN(estimates_are_the_vectors);
    RUN(hand_worked);
    RUN(buckets_hold_a_frame_and_no_more_than_their_room);
    RUN(params_are_checked);
    return CHECK_DONE();
}
