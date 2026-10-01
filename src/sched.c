#include "tsim/sched.h"

#include <stdlib.h>

/* Events live in a pool of slots; the heap orders slot numbers. Each slot remembers where it sits
 * in the heap, which is what makes cancelling O(log n) rather than a search. A slot's generation
 * goes up every time it is freed, so a handle to an event that has gone no longer matches.
 *
 * Slot 0 is never used, which keeps the zero handle invalid. */

struct slot {
    tsim_time when;
    uint64_t seq; /* order of scheduling; breaks ties between events at the same time */
    tsim_event_fn fn;
    void *ctx;
    uint32_t gen;
    uint32_t heap_pos; /* index into heap while pending; NOT_QUEUED otherwise */
    uint32_t next_free;
};

#define NOT_QUEUED UINT32_MAX

struct tsim_sched {
    tsim_time now;
    uint64_t next_seq;
    struct slot *slots;
    uint32_t slot_cap;
    uint32_t free_head; /* 0 when no slot is free */
    uint32_t *heap;
    uint32_t heap_len;
};

struct tsim_sched *tsim_sched_create(void) {
    struct tsim_sched *s = calloc(1, sizeof *s);
    return s;
}

void tsim_sched_destroy(struct tsim_sched *s) {
    if (!s) {
        return;
    }
    free(s->slots);
    free(s->heap);
    free(s);
}

tsim_time tsim_sched_now(const struct tsim_sched *s) { return s->now; }

size_t tsim_sched_size(const struct tsim_sched *s) { return s->heap_len; }

static bool before(const struct tsim_sched *s, uint32_t a, uint32_t b) {
    const struct slot *x = &s->slots[a];
    const struct slot *y = &s->slots[b];
    if (x->when != y->when) {
        return x->when < y->when;
    }
    return x->seq < y->seq;
}

static void place(struct tsim_sched *s, uint32_t pos, uint32_t slot) {
    s->heap[pos] = slot;
    s->slots[slot].heap_pos = pos;
}

static void sift_up(struct tsim_sched *s, uint32_t pos) {
    uint32_t slot = s->heap[pos];
    while (pos > 0) {
        uint32_t parent = (pos - 1) / 2;
        if (!before(s, slot, s->heap[parent])) {
            break;
        }
        place(s, pos, s->heap[parent]);
        pos = parent;
    }
    place(s, pos, slot);
}

static void sift_down(struct tsim_sched *s, uint32_t pos) {
    uint32_t slot = s->heap[pos];
    for (;;) {
        uint32_t child = 2 * pos + 1;
        if (child >= s->heap_len) {
            break;
        }
        if (child + 1 < s->heap_len && before(s, s->heap[child + 1], s->heap[child])) {
            child++;
        }
        if (!before(s, s->heap[child], slot)) {
            break;
        }
        place(s, pos, s->heap[child]);
        pos = child;
    }
    place(s, pos, slot);
}

static bool grow(struct tsim_sched *s) {
    uint32_t cap = s->slot_cap ? s->slot_cap * 2 : 64;
    if (cap <= s->slot_cap || cap > UINT32_MAX / 2) {
        return false;
    }
    struct slot *slots = realloc(s->slots, (size_t)cap * sizeof *slots);
    if (!slots) {
        return false;
    }
    s->slots = slots;
    uint32_t *heap = realloc(s->heap, (size_t)cap * sizeof *heap);
    if (!heap) {
        return false;
    }
    s->heap = heap;

    /* Thread the new slots onto the free list, lowest first. Slot 0 stays out of it. */
    uint32_t first = s->slot_cap ? s->slot_cap : 1;
    for (uint32_t i = cap - 1; i >= first; i--) {
        s->slots[i] = (struct slot){.gen = 1, .heap_pos = NOT_QUEUED, .next_free = s->free_head};
        s->free_head = i;
    }
    s->slot_cap = cap;
    return true;
}

static void release(struct tsim_sched *s, uint32_t slot) {
    struct slot *e = &s->slots[slot];
    e->heap_pos = NOT_QUEUED;
    e->fn = NULL;
    e->ctx = NULL;
    e->gen++;
    if (e->gen == 0) {
        e->gen = 1;
    }
    e->next_free = s->free_head;
    s->free_head = slot;
}

/* Takes the slot at heap position pos out of the heap, keeping the heap ordered. */
static void unlink_at(struct tsim_sched *s, uint32_t pos) {
    uint32_t last = --s->heap_len;
    if (pos == last) {
        return;
    }
    place(s, pos, s->heap[last]);
    if (pos > 0 && before(s, s->heap[pos], s->heap[(pos - 1) / 2])) {
        sift_up(s, pos);
    } else {
        sift_down(s, pos);
    }
}

struct tsim_event tsim_sched_at(struct tsim_sched *s, tsim_time when, tsim_event_fn fn, void *ctx) {
    if (!fn || when < s->now) {
        return (struct tsim_event){0};
    }
    if (!s->free_head && !grow(s)) {
        return (struct tsim_event){0};
    }
    uint32_t slot = s->free_head;
    struct slot *e = &s->slots[slot];
    s->free_head = e->next_free;
    e->when = when;
    e->seq = s->next_seq++;
    e->fn = fn;
    e->ctx = ctx;
    s->heap[s->heap_len] = slot;
    sift_up(s, s->heap_len++);
    return (struct tsim_event){.slot = slot, .gen = e->gen};
}

struct tsim_event tsim_sched_after(struct tsim_sched *s, tsim_time delay, tsim_event_fn fn,
                                   void *ctx) {
    if (delay < 0 || delay > INT64_MAX - s->now) {
        return (struct tsim_event){0};
    }
    return tsim_sched_at(s, s->now + delay, fn, ctx);
}

bool tsim_sched_pending(const struct tsim_sched *s, struct tsim_event ev) {
    return ev.slot != 0 && ev.slot < s->slot_cap && s->slots[ev.slot].gen == ev.gen &&
           s->slots[ev.slot].heap_pos != NOT_QUEUED;
}

bool tsim_sched_cancel(struct tsim_sched *s, struct tsim_event ev) {
    if (!tsim_sched_pending(s, ev)) {
        return false;
    }
    unlink_at(s, s->slots[ev.slot].heap_pos);
    release(s, ev.slot);
    return true;
}

bool tsim_sched_step(struct tsim_sched *s) {
    if (s->heap_len == 0) {
        return false;
    }
    uint32_t slot = s->heap[0];
    struct slot *e = &s->slots[slot];
    tsim_event_fn fn = e->fn;
    void *ctx = e->ctx;
    s->now = e->when;
    unlink_at(s, 0);
    /* Freed before the callback runs: the callback may schedule, and may reuse this slot. */
    release(s, slot);
    fn(s, ctx);
    return true;
}

size_t tsim_sched_run_until(struct tsim_sched *s, tsim_time end) {
    size_t ran = 0;
    while (s->heap_len > 0 && s->slots[s->heap[0]].when <= end) {
        tsim_sched_step(s);
        ran++;
    }
    if (end > s->now) {
        s->now = end;
    }
    return ran;
}
