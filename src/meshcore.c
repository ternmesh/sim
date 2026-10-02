#include "tsim/meshcore.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tsim/nodeset.h"
#include "tsim/rng.h"

/* --- Shared --- */

static uint64_t mix(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

void tsim_meshcore_hash(uint32_t node, uint8_t size, uint8_t *out) {
    uint64_t h = mix(node + 0x9e3779b97f4a7c15ULL);
    for (uint8_t i = 0; i < size; i++) {
        out[i] = (uint8_t)(h >> (8 * i));
    }
}

/* What the radio driver reports as a frame's airtime, as MeshCore uses it: whole milliseconds. */
static uint32_t airtime_ms(const struct tsim_lora *lora, uint32_t len) {
    return (uint32_t)(tsim_lora_airtime(lora, len) / TSIM_MS(1));
}

/* The airtime the routing's delays and timeouts are reckoned from: at estimate_cr, if set. */
static uint32_t estimate_ms(const struct tsim_meshcore_config *c, uint32_t len) {
    struct tsim_lora lora = c->lora;
    if (c->estimate_cr) {
        lora.cr = c->estimate_cr;
    }
    return airtime_ms(&lora, len);
}

/* --- The dispatcher --- */

#define BUDGET_WINDOW_MS 3600000.0
#define BUDGET_RESERVE_MS 100.0
#define BUSY_RETRY TSIM_MS(120)
#define BUSY_MAX TSIM_MS(4000)
#define FRAME_MAX 255

struct mac {
    struct tsim_node *node;
    struct tsim_meshcore_mac_config config;
    struct tsim_rng rng;
    struct tsim_timer *timer;
    tsim_time next_tx;    /* nothing goes before this */
    tsim_time busy_since; /* when the channel was first found busy, or -1 */
    double budget_ms;
    tsim_time budget_at; /* when the budget was last refilled */
    bool on_air;         /* its last frame, until the end of it has been accounted for */
    uint32_t on_air_ms;
};

struct tsim_meshcore_mac_config tsim_meshcore_mac_default(void) {
    return (struct tsim_meshcore_mac_config){.airtime_factor = 1.0};
}

static double duty(const struct mac *m) { return 1.0 / (1.0 + m->config.airtime_factor); }

static void refill(struct mac *m) {
    tsim_time now = tsim_node_now(m->node);
    double full = BUDGET_WINDOW_MS * duty(m);
    m->budget_ms += (double)(now - m->budget_at) / (double)TSIM_MS(1) * duty(m);
    if (m->budget_ms > full) {
        m->budget_ms = full;
    }
    m->budget_at = now;
}

static void wait_until(struct mac *m, tsim_time when) {
    tsim_time now = tsim_node_now(m->node);
    tsim_timer_start(m->timer, when > now ? when - now : 0);
}

/* `ms` of waiting, rounded up: a wait that rounded down could find the budget a hair short again at
 * the same instant, for ever. */
static tsim_time ms_up(double ms) {
    tsim_time t = (tsim_time)ceil(ms * (double)TSIM_MS(1));
    return t > 0 ? t : 1;
}

static void try_send(struct mac *m) {
    const struct tsim_tx *head = tsim_node_head(m->node);
    if (!head || tsim_node_sending(m->node)) {
        return;
    }
    tsim_time now = tsim_node_now(m->node);
    refill(m);
    double needed = airtime_ms(&head->lora, FRAME_MAX) / 2;
    if (m->budget_ms < needed) {
        double ms = (needed - m->budget_ms) / duty(m);
        wait_until(m, now + ms_up(ms));
        return;
    }
    if (now < m->next_tx) {
        wait_until(m, m->next_tx);
        return;
    }
    if (tsim_node_carrier(m->node)) {
        if (m->busy_since < 0) {
            m->busy_since = now;
        }
        if (now - m->busy_since <= BUSY_MAX) {
            m->next_tx = now + (1 + (tsim_time)tsim_rng_below(&m->rng, 3)) * BUSY_RETRY;
            wait_until(m, m->next_tx);
            return;
        }
        /* Busy for too long: the firmware takes the radio to be stuck and sends anyway. */
    }
    m->busy_since = -1;
    uint32_t ms = airtime_ms(&head->lora, head->len);
    if (tsim_node_transmit(m->node)) {
        m->on_air = true;
        m->on_air_ms = ms;
    }
}

static void mac_fire(void *ctx) { try_send(ctx); }

static void mac_kick(void *self) {
    struct mac *m = self;
    if (tsim_node_sending(m->node)) {
        return;
    }
    if (m->on_air) {
        /* The frame has gone: charge it, and send the next straight away if the budget allows. */
        m->on_air = false;
        refill(m);
        m->budget_ms = m->on_air_ms > m->budget_ms ? 0 : m->budget_ms - m->on_air_ms;
        tsim_time now = tsim_node_now(m->node);
        m->next_tx = now;
        if (m->budget_ms < BUDGET_RESERVE_MS) {
            double ms = (BUDGET_RESERVE_MS - m->budget_ms) / duty(m);
            m->next_tx = now + ms_up(ms);
        }
    }
    if (!tsim_timer_pending(m->timer)) {
        try_send(m);
    }
}

static void *mac_create(struct tsim_node *node, const void *config) {
    const struct tsim_meshcore_mac_config *c = config;
    if (!(c->airtime_factor >= 0 && c->airtime_factor <= 1e6) || c->latched_header < 0) {
        return NULL;
    }
    struct mac *m = calloc(1, sizeof *m);
    if (!m) {
        return NULL;
    }
    m->node = node;
    m->config = *c;
    m->busy_since = -1;
    m->budget_ms = BUDGET_WINDOW_MS * duty(m);
    tsim_node_rng(node, TSIM_STREAM_MAC, &m->rng);
    m->timer = tsim_timer_create(node, mac_fire, m);
    if (!m->timer) {
        free(m);
        return NULL;
    }
    tsim_node_hold_header(node, c->latched_header);
    return m;
}

static void mac_destroy(void *self) {
    struct mac *m = self;
    tsim_timer_destroy(m->timer);
    free(m);
}

const struct tsim_mac tsim_meshcore_mac = {
    .name = "meshcore",
    .create = mac_create,
    .destroy = mac_destroy,
    .kick = mac_kick,
};

/* --- Frames --- */

#define ROUTE_TRANSPORT_FLOOD 0x00
#define ROUTE_FLOOD 0x01
#define ROUTE_DIRECT 0x02
#define ROUTE_MASK 0x03
#define TYPE_SHIFT 2
#define TYPE_MASK 0x0F

#define TYPE_TXT 0x02
#define TYPE_ACK 0x03
#define TYPE_ADVERT 0x04
#define TYPE_GRP_TXT 0x05
#define TYPE_PATH 0x08

#define CODES_LEN 4
#define MAC_LEN 2
#define BLOCK 16
#define PLAIN_HEAD 5 /* the timestamp - here the message id - and the flags byte */
#define ACK_LEN 6    /* the acknowledgement's hash, the attempt and a random byte */
#define ADVERT_LEN 121
#define EXTRA_ACK 0x03
#define EXTRA_NONE 0xFF

#define TXT_ACK_DELAY_MS 200
#define PATH_RETURN_DELAY_MS 500
#define PATH_RETRY_DELAY_MS 3000
#define RX_DELAY_MIN_MS 50
#define RX_DELAY_MAX_MS 32000
#define SEEN 160

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t padded(uint32_t len) { return (len + BLOCK - 1) / BLOCK * BLOCK; }

/* A received frame, taken apart. */
struct packet {
    uint8_t header;
    uint8_t type;
    bool flood;
    bool codes;
    uint8_t hash_size;
    uint8_t count; /* path entries */
    const uint8_t *path;
    const uint8_t *payload;
    uint32_t payload_len;
};

static bool parse(const uint8_t *b, uint32_t len, struct packet *p) {
    uint32_t i = 0;
    if (len < 2) {
        return false;
    }
    p->header = b[i++];
    uint8_t route = p->header & ROUTE_MASK;
    p->type = (p->header >> TYPE_SHIFT) & TYPE_MASK;
    p->flood = route == ROUTE_FLOOD || route == ROUTE_TRANSPORT_FLOOD;
    p->codes = route == ROUTE_TRANSPORT_FLOOD || route == 0x03;
    if (p->codes) {
        i += CODES_LEN;
    }
    if (i >= len) {
        return false;
    }
    uint8_t path_len = b[i++];
    if (path_len >> 6 == 3) {
        return false;
    }
    p->hash_size = (uint8_t)((path_len >> 6) + 1);
    p->count = path_len & 63;
    uint32_t path_bytes = (uint32_t)p->count * p->hash_size;
    if (path_bytes > TSIM_MESHCORE_PATH_MAX || i + path_bytes > len) {
        return false;
    }
    p->path = b + i;
    i += path_bytes;
    p->payload = b + i;
    p->payload_len = len - i;
    return p->payload_len <= TSIM_MESHCORE_PAYLOAD_MAX;
}

/* The packet hash: its type and payload, never its path, so every copy of a flood shares one. */
static uint64_t packet_hash(uint8_t type, const uint8_t *payload, uint32_t len) {
    uint64_t h = 0xcbf29ce484222325ULL ^ type;
    for (uint32_t i = 0; i < len; i++) {
        h = (h ^ payload[i]) * 0x100000001b3ULL;
    }
    h = mix(h);
    return h ? h : 1; /* 0 marks an empty slot */
}

/* Stands in for the MAC an encrypted payload carries: only the true sender and receiver agree on
 * it, as only they share the key, and like the firmware's 2 bytes it lets a stranger through once
 * in 65536. */
static uint16_t cipher_mac(uint32_t src, uint32_t dst, const uint8_t *cipher, uint32_t len) {
    uint64_t h = mix((uint64_t)src << 32 | dst);
    for (uint32_t i = 0; i < len; i++) {
        h = (h ^ cipher[i]) * 0x100000001b3ULL;
    }
    return (uint16_t)mix(h);
}

/* What the destination of attempt `attempt` of message `id` from `src` sends back. */
static uint32_t ack_code(uint32_t src, uint32_t id, uint8_t attempt) {
    return (uint32_t)mix((uint64_t)src << 40 ^ (uint64_t)attempt << 32 ^ id ^ 0xac4ac4ULL);
}

/* --- The routing --- */

/* A frame waiting out its delay, and then in the node's queue until it has gone. */
struct held {
    uint64_t hash; /* a flood relay's packet hash, which cancel_heard looks for; 0 for others */
    tsim_time due;
    uint64_t handle; /* 0 while it waits */
    struct tsim_tx tx;
};

/* A received flood waiting out its receive delay before it is looked at. */
struct inbound {
    tsim_time due;
    uint64_t order;
    uint64_t hash;
    bool heard_again; /* cancel_heard: another copy came meanwhile, so it is not relayed */
    uint32_t len;
    uint8_t bytes[FRAME_MAX];
};

/* The path to a contact: a path length byte, as on the air, and the hashes. */
struct path {
    uint32_t node; /* the contact, plus 1; 0 is an empty slot */
    uint8_t len;
    uint8_t bytes[TSIM_MESHCORE_PATH_MAX];
};

/* A message of this node's waiting for an acknowledgement. */
struct awaiting {
    struct router *owner;
    struct awaiting *next;
    uint32_t id;
    uint32_t dst;
    uint8_t attempt;
    bool direct; /* the current attempt went direct */
    struct tsim_timer *timer;
    uint32_t len;
    uint8_t content[FRAME_MAX];
};

struct router {
    struct tsim_node *node;
    uint32_t self;
    uint32_t nodes;
    uint8_t hash[3];
    bool relay;
    struct tsim_meshcore_config config;
    struct tsim_rng rng;
    uint64_t seen[SEEN];
    unsigned seen_next;
    struct held *held;
    size_t held_count;
    size_t held_cap;
    struct tsim_timer *out_timer;
    struct inbound *in;
    size_t in_count;
    size_t in_cap;
    uint64_t in_order;
    struct tsim_timer *in_timer;
    bool hold_relay;    /* the flood being looked at was heard again while it waited */
    struct path *paths; /* open addressing, a power of two long */
    size_t paths_cap;
    size_t paths_count;
    struct awaiting *awaiting;
    struct tsim_timer *advert_timer;
};

struct tsim_meshcore_config tsim_meshcore_default(uint16_t channel, const struct tsim_lora *lora,
                                                  double tx_dbm) {
    return (struct tsim_meshcore_config){
        .channel = channel,
        .lora = *lora,
        .tx_dbm = tx_dbm,
        .relays = "all",
        .hash_size = 1,
        .scoped = true,
        .flood_max = 64,
        .rx_delay_base = 0,
        .tx_delay_factor = 0.5,
        .direct_tx_delay_factor = 0.3,
        .retries = 3,
        .advert_interval = TSIM_S(120),
    };
}

bool tsim_meshcore_relays_valid(const char *spec) {
    return strlen(spec) < sizeof((struct tsim_meshcore_config *)0)->relays &&
           tsim_nodeset_contains(spec, 0) >= 0;
}

static bool seen(const struct router *r, uint64_t hash) {
    for (unsigned i = 0; i < SEEN; i++) {
        if (r->seen[i] == hash) {
            return true;
        }
    }
    return false;
}

static void mark_seen(struct router *r, uint64_t hash) {
    r->seen[r->seen_next] = hash;
    r->seen_next = (r->seen_next + 1) % SEEN;
}

/* --- Paths to contacts --- */

static size_t path_slot(uint32_t node, size_t cap) { return (size_t)mix(node) & (cap - 1); }

static struct path *path_to(struct router *r, uint32_t node) {
    if (!r->paths_cap) {
        return NULL;
    }
    size_t at = path_slot(node, r->paths_cap);
    while (r->paths[at].node) {
        if (r->paths[at].node == node + 1) {
            return r->paths[at].len != 0xFF ? &r->paths[at] : NULL;
        }
        at = (at + 1) & (r->paths_cap - 1);
    }
    return NULL;
}

/* Stores a path, or forgets one with a len of 0xFF. Forgetting keeps the slot, marked unknown. */
static void set_path(struct router *r, uint32_t node, uint8_t len, const uint8_t *bytes) {
    if (2 * (r->paths_count + 1) > r->paths_cap) {
        size_t cap = r->paths_cap ? 2 * r->paths_cap : 16;
        struct path *grown = calloc(cap, sizeof *grown);
        if (!grown) {
            return; /* out of memory: the path is not learned, and the next message floods */
        }
        for (size_t i = 0; i < r->paths_cap; i++) {
            if (r->paths[i].node) {
                size_t at = path_slot(r->paths[i].node - 1, cap);
                while (grown[at].node) {
                    at = (at + 1) & (cap - 1);
                }
                grown[at] = r->paths[i];
            }
        }
        free(r->paths);
        r->paths = grown;
        r->paths_cap = cap;
    }
    size_t at = path_slot(node, r->paths_cap);
    while (r->paths[at].node && r->paths[at].node != node + 1) {
        at = (at + 1) & (r->paths_cap - 1);
    }
    if (!r->paths[at].node) {
        r->paths[at].node = node + 1;
        r->paths_count++;
    }
    r->paths[at].len = len;
    if (len != 0xFF) {
        memcpy(r->paths[at].bytes, bytes, (size_t)(len & 63) * ((len >> 6) + 1));
    }
}

/* --- Sending --- */

static void arm_out(struct router *r) {
    tsim_time next = -1;
    for (size_t i = 0; i < r->held_count; i++) {
        if (r->held[i].handle == 0 && (next < 0 || r->held[i].due < next)) {
            next = r->held[i].due;
        }
    }
    if (next < 0) {
        tsim_timer_stop(r->out_timer);
    } else {
        tsim_time now = tsim_node_now(r->node);
        tsim_timer_start(r->out_timer, next > now ? next - now : 0);
    }
}

/* Takes a frame out of the held list keeping the rest in order, since equals go first-queued first.
 */
static void drop_held(struct router *r, size_t i) {
    r->held_count--;
    memmove(&r->held[i], &r->held[i + 1], (r->held_count - i) * sizeof *r->held);
}

static void out_fire(void *ctx) {
    struct router *r = ctx;
    tsim_time now = tsim_node_now(r->node);
    for (size_t i = 0; i < r->held_count;) {
        struct held *h = &r->held[i];
        if (h->handle == 0 && h->due <= now) {
            h->handle = tsim_node_send(r->node, &h->tx);
            if (h->handle == 0) {
                drop_held(r, i); /* the queue was full: dropped, as the firmware drops it */
                continue;
            }
        }
        i++;
    }
    arm_out(r);
}

/* Queues `tx` to go `delay_ms` from now at MeshCore priority `priority`, 0 the most urgent. */
static void schedule(struct router *r, struct tsim_tx *tx, uint32_t delay_ms, uint8_t priority,
                     uint64_t relay_hash) {
    tx->priority = (uint8_t)(255 - priority);
    if (r->held_count == r->held_cap) {
        size_t cap = r->held_cap ? 2 * r->held_cap : 8;
        struct held *grown = realloc(r->held, cap * sizeof *grown);
        if (!grown) {
            return; /* out of memory: not sent, as when the firmware's pool is empty */
        }
        r->held = grown;
        r->held_cap = cap;
    }
    r->held[r->held_count++] = (struct held){
        .hash = relay_hash,
        .due = tsim_node_now(r->node) + (tsim_time)delay_ms * TSIM_MS(1),
        .tx = *tx,
    };
    arm_out(r);
}

/* Cancel_heard: another node has sent this flood. If a copy of it is waiting out its receive
 * delay, neither that copy nor this one is to be relayed - returning true - and when queuing too,
 * a relay of it still waiting is dropped. */
static bool heard_again(struct router *r, uint64_t hash) {
    bool waiting = false;
    for (size_t i = 0; i < r->in_count; i++) {
        if (r->in[i].hash == hash) {
            r->in[i].heard_again = true;
            waiting = true;
        }
    }
    if (r->config.cancel_heard != TSIM_MESHCORE_CANCEL_QUEUED) {
        return waiting;
    }
    for (size_t i = 0; i < r->held_count;) {
        struct held *h = &r->held[i];
        if (h->hash == hash && (h->handle == 0 || tsim_node_cancel(r->node, h->handle))) {
            drop_held(r, i);
            continue;
        }
        i++;
    }
    arm_out(r);
    return waiting;
}

/* Starts a frame: header, region codes on a scoped flood, path. Returns where the payload goes. */
static uint32_t frame_start(const struct router *r, struct tsim_tx *tx, bool flood, uint8_t type,
                            uint8_t path_len, const uint8_t *path, uint64_t hash,
                            enum tsim_purpose purpose) {
    *tx = (struct tsim_tx){
        .channel = r->config.channel,
        .lora = r->config.lora,
        .tx_dbm = r->config.tx_dbm,
        .purpose = purpose,
    };
    bool codes = flood && r->config.scoped;
    uint8_t route = !flood ? ROUTE_DIRECT : codes ? ROUTE_TRANSPORT_FLOOD : ROUTE_FLOOD;
    uint32_t i = 0;
    tx->bytes[i++] = (uint8_t)(type << TYPE_SHIFT | route);
    if (codes) {
        put32(tx->bytes + i, (uint32_t)(hash & 0xFFFF)); /* the region's code, and 0 */
        i += CODES_LEN;
    }
    tx->bytes[i++] = path_len;
    uint32_t path_bytes = (uint32_t)(path_len & 63) * ((path_len >> 6) + 1);
    if (path_bytes > 0) {
        memcpy(tx->bytes + i, path, path_bytes);
    }
    return i + path_bytes;
}

static uint8_t empty_path(const struct router *r) {
    return (uint8_t)((r->config.hash_size - 1) << 6);
}

/* Sends a packet this node made: by flood, or direct along `path`. */
static void send_own(struct router *r, bool flood, uint8_t type, const uint8_t *payload,
                     uint32_t len, const struct path *path, uint32_t delay_ms, uint8_t priority,
                     enum tsim_purpose purpose, uint64_t carries, uint32_t carries_at) {
    uint64_t hash = packet_hash(type, payload, len);
    struct tsim_tx tx;
    uint32_t at = frame_start(r, &tx, flood, type, flood ? empty_path(r) : path->len,
                              flood ? NULL : path->bytes, hash, purpose);
    if (at + len > FRAME_MAX) {
        return;
    }
    memcpy(tx.bytes + at, payload, len);
    tx.len = at + len;
    if (carries) {
        tx.carries = carries;
        tx.carries_at = at + carries_at;
    }
    mark_seen(r, hash);
    schedule(r, &tx, delay_ms, priority, 0);
}

/* An encrypted payload for `dst`: destination and source hashes, the MAC, then `plain` padded. */
static uint32_t seal(const struct router *r, uint32_t dst, const uint8_t *plain, uint32_t len,
                     uint8_t *out) {
    uint8_t dst_hash;
    tsim_meshcore_hash(dst, 1, &dst_hash);
    uint32_t cipher_len = padded(len);
    out[0] = dst_hash;
    out[1] = r->hash[0];
    memset(out + 4, 0, cipher_len);
    memcpy(out + 4, plain, len);
    uint16_t mac = cipher_mac(r->self, dst, out + 4, cipher_len);
    out[2] = (uint8_t)mac;
    out[3] = (uint8_t)(mac >> 8);
    return 4 + cipher_len;
}

/* Which contact sealed this payload for this node, or -1: one whose hash matches and whose MAC
 * checks, as the firmware tries each contact the source hash could be. */
static int64_t opener(const struct router *r, const uint8_t *payload, uint32_t len) {
    if (len <= 4 || payload[0] != r->hash[0]) {
        return -1;
    }
    uint16_t mac = (uint16_t)(payload[2] | payload[3] << 8);
    for (uint32_t j = 0; j < r->nodes; j++) {
        uint8_t h;
        tsim_meshcore_hash(j, 1, &h);
        if (j != r->self && h == payload[1] &&
            cipher_mac(j, r->self, payload + 4, len - 4) == mac) {
            return j;
        }
    }
    return -1;
}

/* A path return to `to`: the path a packet from it took to here, and an acknowledgement or not. */
static void send_path_return(struct router *r, uint32_t to, uint8_t path_len, const uint8_t *path,
                             const uint8_t *ack, bool flood, const struct path *via,
                             uint32_t delay_ms) {
    uint8_t plain[1 + TSIM_MESHCORE_PATH_MAX + 1 + ACK_LEN];
    uint32_t path_bytes = (uint32_t)(path_len & 63) * ((path_len >> 6) + 1);
    uint32_t n = 0;
    plain[n++] = path_len;
    memcpy(plain + n, path, path_bytes);
    n += path_bytes;
    if (ack) {
        plain[n++] = EXTRA_ACK;
        memcpy(plain + n, ack, ACK_LEN);
        n += ACK_LEN;
    } else {
        plain[n++] = EXTRA_NONE;
        for (int i = 0; i < 4; i++) {
            plain[n++] = (uint8_t)tsim_rng_below(&r->rng, 256);
        }
    }
    if (path_bytes + (ack ? ACK_LEN : 0) + 5 > TSIM_MESHCORE_PAYLOAD_MAX - 2 - BLOCK) {
        return; /* too long, as the firmware refuses it */
    }
    uint8_t payload[TSIM_MESHCORE_PAYLOAD_MAX + BLOCK];
    uint32_t len = seal(r, to, plain, n, payload);
    send_own(r, flood, TYPE_PATH, payload, len, via, delay_ms, flood ? 2 : 1, TSIM_PURPOSE_CONTROL,
             0, 0);
}

static void send_ack(struct router *r, uint32_t to, const uint8_t *ack) {
    const struct path *via = path_to(r, to);
    send_own(r, via == NULL, TYPE_ACK, ack, ACK_LEN, via, TXT_ACK_DELAY_MS, via ? 0 : 1,
             TSIM_PURPOSE_CONTROL, 0, 0);
}

/* --- Messages of this node's --- */

static void forget_awaiting(struct awaiting *a) {
    struct router *r = a->owner;
    for (struct awaiting **p = &r->awaiting; *p; p = &(*p)->next) {
        if (*p == a) {
            *p = a->next;
            break;
        }
    }
    tsim_timer_destroy(a->timer);
    free(a);
}

static void send_attempt(struct awaiting *a) {
    struct router *r = a->owner;
    uint8_t plain[PLAIN_HEAD + FRAME_MAX];
    put32(plain, a->id);
    plain[4] = a->attempt;
    memcpy(plain + PLAIN_HEAD, a->content, a->len);
    uint8_t payload[TSIM_MESHCORE_PAYLOAD_MAX + BLOCK];
    uint32_t len = seal(r, a->dst, plain, PLAIN_HEAD + a->len, payload);
    const struct path *via = path_to(r, a->dst);
    a->direct = via != NULL;
    uint32_t raw = 1 +
                   (via                ? 0
                    : r->config.scoped ? CODES_LEN
                                       : 0) +
                   1 + (via ? (uint32_t)(via->len & 63) * ((via->len >> 6) + 1) : 0) + len;
    uint32_t t = estimate_ms(&r->config, raw);
    uint32_t timeout = via ? 500 + (t * 6 + 250) * ((via->len & 63) + 1u) : 500 + 16 * t;
    send_own(r, !via, TYPE_TXT, payload, len, via, 0, via ? 0 : 1, TSIM_PURPOSE_DATA, a->id,
             4 + PLAIN_HEAD);
    tsim_timer_start(a->timer, (tsim_time)timeout * TSIM_MS(1));
}

static void ack_timeout(void *ctx) {
    struct awaiting *a = ctx;
    struct router *r = a->owner;
    if (a->attempt >= r->config.retries) {
        tsim_node_finished(r->node, a->id);
        forget_awaiting(a);
        return;
    }
    if (a->direct) {
        set_path(r, a->dst, 0xFF, NULL); /* the path failed: the next attempt floods */
    }
    a->attempt++;
    send_attempt(a);
}

/* An acknowledgement has come: whether it was for one of this node's messages. */
static struct awaiting *acknowledged(struct router *r, uint32_t code) {
    for (struct awaiting *a = r->awaiting; a; a = a->next) {
        for (unsigned i = 0; i <= a->attempt; i++) {
            if (ack_code(r->self, a->id, (uint8_t)i) == code) {
                return a;
            }
        }
    }
    return NULL;
}

static bool finish_acked(struct router *r, uint32_t code) {
    struct awaiting *a = acknowledged(r, code);
    if (!a) {
        return false;
    }
    tsim_node_finished(r->node, a->id);
    forget_awaiting(a);
    return true;
}

static bool router_originate(void *self, const struct tsim_message *msg) {
    struct router *r = self;
    uint32_t len = msg->len;
    /* The firmware's limit, with room for a full path in front. */
    if (msg->id > UINT32_MAX || 4 + padded(PLAIN_HEAD + len) > TSIM_MESHCORE_PAYLOAD_MAX ||
        1 + CODES_LEN + 1 + TSIM_MESHCORE_PATH_MAX + 4 + padded(PLAIN_HEAD + len) > FRAME_MAX) {
        return false;
    }
    if (msg->dst == TSIM_BROADCAST) {
        uint8_t payload[TSIM_MESHCORE_PAYLOAD_MAX + BLOCK];
        payload[0] = 0x11; /* the public channel's hash */
        uint32_t cipher_len = padded(PLAIN_HEAD + len);
        memset(payload + 3, 0, cipher_len);
        put32(payload + 3, (uint32_t)msg->id);
        memcpy(payload + 3 + PLAIN_HEAD, msg->content, len);
        uint16_t mac = cipher_mac(0, 0, payload + 3, cipher_len);
        payload[1] = (uint8_t)mac;
        payload[2] = (uint8_t)(mac >> 8);
        send_own(r, true, TYPE_GRP_TXT, payload, 3 + cipher_len, NULL, 0, 1, TSIM_PURPOSE_DATA,
                 msg->id, 3 + PLAIN_HEAD);
        tsim_node_finished(r->node, msg->id);
        return true;
    }
    struct awaiting *a = calloc(1, sizeof *a);
    if (a) {
        a->timer = tsim_timer_create(r->node, ack_timeout, a);
    }
    if (!a || !a->timer) {
        free(a);
        return false; /* out of memory */
    }
    a->owner = r;
    a->id = (uint32_t)msg->id;
    a->dst = msg->dst;
    a->len = len;
    memcpy(a->content, msg->content, len);
    a->next = r->awaiting;
    r->awaiting = a;
    send_attempt(a);
    return true;
}

/* --- Receiving --- */

/* Relays a flood onward with this node's hash on the path, unless it may not. */
static void relay_flood(struct router *r, const uint8_t *bytes, const struct packet *p,
                        uint64_t hash, enum tsim_purpose purpose, uint64_t carries,
                        uint32_t carries_at) {
    uint32_t path_bytes = (uint32_t)p->count * p->hash_size;
    /* The count is six bits. The firmware would write a 64th hop into the hash size's bits and
     * corrupt its own frame; this stops at 63 instead. */
    if (!r->relay || r->hold_relay || p->count >= r->config.flood_max || p->count >= 63 ||
        path_bytes + p->hash_size > TSIM_MESHCORE_PATH_MAX) {
        return;
    }
    struct tsim_tx tx = {
        .channel = r->config.channel,
        .lora = r->config.lora,
        .tx_dbm = r->config.tx_dbm,
        .purpose = purpose,
    };
    uint32_t i = 0;
    tx.bytes[i++] = p->header;
    if (p->codes) {
        memcpy(tx.bytes + i, bytes + 1, CODES_LEN);
        i += CODES_LEN;
    }
    tx.bytes[i++] = (uint8_t)((p->hash_size - 1) << 6 | (p->count + 1));
    memcpy(tx.bytes + i, p->path, path_bytes);
    i += path_bytes;
    memcpy(tx.bytes + i, r->hash, p->hash_size);
    i += p->hash_size;
    uint32_t at = i;
    memcpy(tx.bytes + i, p->payload, p->payload_len);
    tx.len = i + p->payload_len;
    if (carries) {
        tx.carries = carries;
        tx.carries_at = at + carries_at;
    }
    /* The firmware's own length: path, payload and 2, never the region codes the relay carries
     * (MyMesh::getRetransmitDelay). */
    uint32_t t =
        (uint32_t)(estimate_ms(&r->config, path_bytes + p->hash_size + p->payload_len + 2) *
                   r->config.tx_delay_factor);
    uint32_t delay = (uint32_t)tsim_rng_below(&r->rng, 5 * (uint64_t)t + 1);
    schedule(r, &tx, delay, (uint8_t)(p->count + 1), hash);
}

/* Relays a direct packet on, this node's hash taken off the front of the path. */
static void relay_direct(struct router *r, const struct packet *p, uint32_t delay_ms,
                         enum tsim_purpose purpose, uint64_t carries, uint32_t carries_at) {
    struct tsim_tx tx = {
        .channel = r->config.channel,
        .lora = r->config.lora,
        .tx_dbm = r->config.tx_dbm,
        .purpose = purpose,
    };
    uint32_t rest = (uint32_t)(p->count - 1) * p->hash_size;
    uint32_t i = 0;
    tx.bytes[i++] = p->header;
    tx.bytes[i++] = (uint8_t)((p->hash_size - 1) << 6 | (p->count - 1));
    memcpy(tx.bytes + i, p->path + p->hash_size, rest);
    i += rest;
    uint32_t at = i;
    memcpy(tx.bytes + i, p->payload, p->payload_len);
    tx.len = i + p->payload_len;
    if (carries) {
        tx.carries = carries;
        tx.carries_at = at + carries_at;
    }
    schedule(r, &tx, delay_ms, 0, 0);
}

static void on_txt(struct router *r, const struct packet *p, uint32_t from) {
    const uint8_t *cipher = p->payload + 4;
    uint32_t id = get32(cipher);
    uint8_t attempt = cipher[4];
    tsim_node_deliver(r->node, id);
    uint8_t ack[ACK_LEN];
    put32(ack, ack_code(from, id, attempt));
    ack[4] = attempt;
    ack[5] = (uint8_t)tsim_rng_below(&r->rng, 256);
    if (p->flood) {
        send_path_return(r, from, (uint8_t)((p->hash_size - 1) << 6 | p->count), p->path, ack, true,
                         NULL, TXT_ACK_DELAY_MS);
    } else {
        send_ack(r, from, ack);
    }
}

static void on_path(struct router *r, const struct packet *p, uint32_t from) {
    const uint8_t *plain = p->payload + 4;
    uint32_t avail = p->payload_len - 4;
    uint8_t path_len = plain[0];
    uint32_t path_bytes = (uint32_t)(path_len & 63) * ((path_len >> 6) + 1);
    if (path_len >> 6 == 3 || path_bytes > TSIM_MESHCORE_PATH_MAX || 1 + path_bytes + 1 > avail) {
        return;
    }
    set_path(r, from, path_len, plain + 1);
    const uint8_t *extra = plain + 1 + path_bytes;
    if (extra[0] == EXTRA_ACK && 1 + path_bytes + 1 + 4 <= avail) {
        finish_acked(r, get32(extra + 1));
    }
    const struct path *via = path_to(r, from);
    if (p->flood && via) {
        send_path_return(r, from, (uint8_t)((p->hash_size - 1) << 6 | p->count), p->path, NULL,
                         false, via, PATH_RETURN_DELAY_MS);
    }
}

static void process(struct router *r, const uint8_t *bytes, uint32_t len) {
    struct packet p;
    if (!parse(bytes, len, &p)) {
        return;
    }
    uint64_t hash = packet_hash(p.type, p.payload, p.payload_len);

    if (!p.flood && p.count > 0) {
        if (p.type == TYPE_ACK && p.payload_len >= 4) {
            finish_acked(r, get32(p.payload)); /* an acknowledgement heard early, on its way */
        }
        if (!r->relay || memcmp(p.path, r->hash, p.hash_size) != 0 || seen(r, hash)) {
            return;
        }
        mark_seen(r, hash);
        if (p.type == TYPE_ACK) {
            relay_direct(r, &p, 0, TSIM_PURPOSE_CONTROL, 0, 0);
            return;
        }
        uint32_t rest = (uint32_t)(p.count - 1) * p.hash_size;
        /* As for a flood relay, the firmware's length (MyMesh::getDirectRetransmitDelay). */
        uint32_t t = (uint32_t)(estimate_ms(&r->config, rest + p.payload_len + 2) *
                                r->config.direct_tx_delay_factor);
        uint32_t delay = (uint32_t)tsim_rng_below(&r->rng, 5 * (uint64_t)t + 1);
        bool txt = p.type == TYPE_TXT && p.payload_len >= 4 + PLAIN_HEAD;
        relay_direct(r, &p, delay, txt ? TSIM_PURPOSE_RELAY : TSIM_PURPOSE_CONTROL,
                     txt ? get32(p.payload + 4) : 0, 4 + PLAIN_HEAD);
        return;
    }

    if (seen(r, hash)) {
        return;
    }
    switch (p.type) {
    case TYPE_ACK: {
        if (p.payload_len < 4) {
            return;
        }
        mark_seen(r, hash);
        struct awaiting *a = acknowledged(r, get32(p.payload));
        if (a) {
            uint32_t to = a->dst;
            finish_acked(r, get32(p.payload));
            const struct path *via = path_to(r, to);
            if (p.flood && via) {
                /* It has a path back but flooded: it may not have the return path. */
                send_path_return(r, to, (uint8_t)((p.hash_size - 1) << 6 | p.count), p.path, NULL,
                                 false, via, PATH_RETRY_DELAY_MS);
            }
            return; /* for this node: not relayed */
        }
        if (p.flood) {
            relay_flood(r, bytes, &p, hash, TSIM_PURPOSE_CONTROL, 0, 0);
        }
        return;
    }
    case TYPE_TXT:
    case TYPE_PATH: {
        if (p.payload_len <= 4 + MAC_LEN) {
            return;
        }
        mark_seen(r, hash);
        bool txt = p.type == TYPE_TXT && p.payload_len >= 4 + PLAIN_HEAD;
        int64_t from = opener(r, p.payload, p.payload_len);
        if (from >= 0) {
            if (txt) {
                on_txt(r, &p, (uint32_t)from);
            } else if (p.type == TYPE_PATH) {
                on_path(r, &p, (uint32_t)from);
            }
            return; /* for this node: not relayed */
        }
        if (p.flood) {
            relay_flood(r, bytes, &p, hash, txt ? TSIM_PURPOSE_RELAY : TSIM_PURPOSE_CONTROL,
                        txt ? get32(p.payload + 4) : 0, 4 + PLAIN_HEAD);
        }
        return;
    }
    case TYPE_GRP_TXT: {
        if (p.payload_len < 3 + PLAIN_HEAD) {
            return;
        }
        mark_seen(r, hash);
        uint32_t id = get32(p.payload + 3);
        tsim_node_deliver(r->node, id);
        if (p.flood) {
            relay_flood(r, bytes, &p, hash, TSIM_PURPOSE_RELAY, id, 3 + PLAIN_HEAD);
        }
        return;
    }
    case TYPE_ADVERT:
        mark_seen(r, hash);
        if (p.flood) {
            relay_flood(r, bytes, &p, hash, TSIM_PURPOSE_ANNOUNCE, 0, 0);
        }
        return;
    default:
        return;
    }
}

static void arm_in(struct router *r) {
    tsim_time next = -1;
    for (size_t i = 0; i < r->in_count; i++) {
        if (next < 0 || r->in[i].due < next) {
            next = r->in[i].due;
        }
    }
    if (next < 0) {
        tsim_timer_stop(r->in_timer);
    } else {
        tsim_time now = tsim_node_now(r->node);
        tsim_timer_start(r->in_timer, next > now ? next - now : 0);
    }
}

/* The floods whose receive delay is up, earliest first. */
static void in_fire(void *ctx) {
    struct router *r = ctx;
    tsim_time now = tsim_node_now(r->node);
    for (;;) {
        size_t best = r->in_count;
        for (size_t i = 0; i < r->in_count; i++) {
            const struct inbound *in = &r->in[i];
            if (in->due <= now && (best == r->in_count || in->due < r->in[best].due ||
                                   (in->due == r->in[best].due && in->order < r->in[best].order))) {
                best = i;
            }
        }
        if (best == r->in_count) {
            break;
        }
        struct inbound in = r->in[best];
        r->in[best] = r->in[--r->in_count];
        r->hold_relay = in.heard_again;
        process(r, in.bytes, in.len);
        r->hold_relay = false;
    }
    arm_in(r);
}

/* How long the firmware holds a received flood before looking at it, in milliseconds. */
static uint32_t rx_delay(const struct router *r, double snr_db, uint32_t len) {
    if (r->config.rx_delay_base <= 0) {
        return 0;
    }
    static const double floor_db[] = {-7.5, -10, -12.5, -15, -17.5, -20};
    uint8_t sf = r->config.lora.sf;
    double score = 0;
    if (sf >= 7 && sf <= 12 && snr_db >= floor_db[sf - 7]) {
        score = (snr_db - floor_db[sf - 7]) / 10.0 * (1.0 - len / 256.0);
        score = score < 0 ? 0 : score > 1 ? 1 : score;
    }
    double d = (pow(r->config.rx_delay_base, 0.85 - score) - 1.0) * estimate_ms(&r->config, len);
    return d < RX_DELAY_MIN_MS ? 0 : d > RX_DELAY_MAX_MS ? RX_DELAY_MAX_MS : (uint32_t)d;
}

static void router_rx(void *self, const struct tsim_rx *rx) {
    struct router *r = self;
    struct packet p;
    if (!parse(rx->bytes, rx->len, &p)) {
        return;
    }
    if (p.flood) {
        uint64_t hash = packet_hash(p.type, p.payload, p.payload_len);
        bool again = r->config.cancel_heard != TSIM_MESHCORE_CANCEL_NO && heard_again(r, hash);
        uint32_t delay = rx_delay(r, rx->snr_db, rx->len);
        if (delay > 0) {
            if (r->in_count == r->in_cap) {
                size_t cap = r->in_cap ? 2 * r->in_cap : 8;
                struct inbound *grown = realloc(r->in, cap * sizeof *grown);
                if (!grown) {
                    return; /* out of memory: dropped, as the firmware drops it */
                }
                r->in = grown;
                r->in_cap = cap;
            }
            struct inbound *in = &r->in[r->in_count++];
            in->due = tsim_node_now(r->node) + (tsim_time)delay * TSIM_MS(1);
            in->order = r->in_order++;
            in->hash = hash;
            in->heard_again = again;
            in->len = rx->len;
            memcpy(in->bytes, rx->bytes, rx->len);
            arm_in(r);
            return;
        }
        r->hold_relay = again;
        process(r, rx->bytes, rx->len);
        r->hold_relay = false;
        return;
    }
    process(r, rx->bytes, rx->len);
}

static void router_tx_done(void *self, uint64_t handle) {
    struct router *r = self;
    for (size_t i = 0; i < r->held_count; i++) {
        if (r->held[i].handle == handle) {
            drop_held(r, i);
            return;
        }
    }
}

/* --- Adverts --- */

static void advert_fire(void *ctx) {
    struct router *r = ctx;
    uint8_t payload[ADVERT_LEN] = {0};
    put32(payload, r->self); /* the public key */
    memcpy(payload + 4, r->hash, sizeof r->hash);
    put32(payload + 32, (uint32_t)(tsim_node_now(r->node) / TSIM_S(1))); /* the timestamp */
    for (int i = 36; i < 100; i++) {
        payload[i] = (uint8_t)tsim_rng_below(&r->rng, 256); /* the signature */
    }
    struct path none = {.len = empty_path(r)};
    send_own(r, false, TYPE_ADVERT, payload, ADVERT_LEN, &none, 0, 0, TSIM_PURPOSE_ANNOUNCE, 0, 0);
    tsim_timer_start(r->advert_timer, r->config.advert_interval);
}

static void router_start(void *self) {
    struct router *r = self;
    if (r->advert_timer) {
        tsim_timer_start(r->advert_timer,
                         (tsim_time)tsim_rng_below(&r->rng, (uint64_t)r->config.advert_interval));
    }
}

/* --- Life --- */

static void *router_create(struct tsim_node *node, const void *config) {
    const struct tsim_meshcore_config *c = config;
    int relay = tsim_nodeset_contains(c->relays, tsim_node_index(node));
    if (relay < 0 || c->hash_size < 1 || c->hash_size > 3 || c->flood_max < 1 ||
        c->flood_max > 64 || !(c->rx_delay_base >= 0 && c->rx_delay_base <= 20) ||
        !(c->tx_delay_factor >= 0 && c->tx_delay_factor <= 2) ||
        !(c->direct_tx_delay_factor >= 0 && c->direct_tx_delay_factor <= 2) ||
        c->advert_interval < 0 || c->estimate_cr > 4 ||
        (unsigned)c->cancel_heard > TSIM_MESHCORE_CANCEL_QUEUED) {
        return NULL;
    }
    struct router *r = calloc(1, sizeof *r);
    if (!r) {
        return NULL;
    }
    r->node = node;
    r->self = tsim_node_index(node);
    r->nodes = tsim_node_count(node);
    r->relay = relay == 1;
    r->config = *c;
    tsim_meshcore_hash(r->self, 3, r->hash);
    tsim_node_rng(node, TSIM_STREAM_ROUTING, &r->rng);
    r->out_timer = tsim_timer_create(node, out_fire, r);
    r->in_timer = tsim_timer_create(node, in_fire, r);
    if (r->relay && c->advert_interval > 0) {
        r->advert_timer = tsim_timer_create(node, advert_fire, r);
    }
    if (!r->out_timer || !r->in_timer || (r->relay && c->advert_interval > 0 && !r->advert_timer)) {
        tsim_timer_destroy(r->out_timer);
        tsim_timer_destroy(r->in_timer);
        tsim_timer_destroy(r->advert_timer);
        free(r);
        return NULL;
    }
    return r;
}

static void router_destroy(void *self) {
    struct router *r = self;
    while (r->awaiting) {
        forget_awaiting(r->awaiting);
    }
    tsim_timer_destroy(r->out_timer);
    tsim_timer_destroy(r->in_timer);
    tsim_timer_destroy(r->advert_timer);
    free(r->held);
    free(r->in);
    free(r->paths);
    free(r);
}

const struct tsim_routing tsim_meshcore = {
    .name = "meshcore",
    .create = router_create,
    .destroy = router_destroy,
    .start = router_start,
    .originate = router_originate,
    .rx = router_rx,
    .tx_done = router_tx_done,
    .reports_finished = true,
};
