#include "tsim/scenario.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tsim/baseline.h"
#include "tsim/distvec.h"
#include "tsim/meshcore.h"
#include "tsim/meshtastic.h"
#include "tsim/nodeset.h"
#include "tsim/place.h"
#include "tsim/sched.h"

/* --- Values --- */

static const char *skip_space(const char *s) {
    while (isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

static bool parse_u64(const char *v, uint64_t max, uint64_t *out) {
    if (!isdigit((unsigned char)*v)) {
        return false; /* strtoull would take "-1" */
    }
    char *end;
    errno = 0;
    unsigned long long x = strtoull(v, &end, 10);
    /* Past UINT64_MAX, strtoull answers UINT64_MAX, which is not over a max of UINT64_MAX. */
    if (errno == ERANGE || *skip_space(end) != '\0' || x > max) {
        return false;
    }
    *out = x;
    return true;
}

static bool parse_double(const char *v, double *out) {
    char *end;
    double x = strtod(v, &end);
    if (end == v || *skip_space(end) != '\0' || x != x || x - x != 0) {
        return false; /* empty, trailing text, NaN or infinite */
    }
    *out = x;
    return true;
}

/* A non-negative time with a unit. */
static bool parse_time(const char *v, tsim_time *out) {
    static const struct {
        const char *unit;
        double ns;
    } units[] = {{"ns", 1}, {"us", 1e3}, {"ms", 1e6}, {"s", 1e9}, {"min", 60e9}, {"h", 3600e9}};
    char *end;
    double x = strtod(v, &end);
    if (end == v || !(x >= 0)) {
        return false;
    }
    const char *unit = skip_space(end);
    for (size_t i = 0; i < sizeof units / sizeof units[0]; i++) {
        size_t len = strlen(units[i].unit);
        if (strncmp(unit, units[i].unit, len) == 0 && *skip_space(unit + len) == '\0') {
            double ns = x * units[i].ns;
            if (!(ns < 9e18)) {
                return false;
            }
            *out = (tsim_time)(ns + 0.5);
            return true;
        }
    }
    return false;
}

/* "32" or "16..64". */
static bool parse_range(const char *v, uint32_t max, uint32_t *lo, uint32_t *hi) {
    const char *dots = strstr(v, "..");
    uint64_t a, b;
    if (!dots) {
        if (!parse_u64(v, max, &a)) {
            return false;
        }
        *lo = *hi = (uint32_t)a;
        return true;
    }
    char first[32];
    size_t len = (size_t)(dots - v);
    if (len == 0 || len >= sizeof first) {
        return false;
    }
    memcpy(first, v, len);
    first[len] = '\0';
    if (!parse_u64(first, max, &a) || !parse_u64(skip_space(dots + 2), max, &b) || a > b) {
        return false;
    }
    *lo = (uint32_t)a;
    *hi = (uint32_t)b;
    return true;
}

/* "5000 x 3000". */
static bool parse_area(const char *v, double *w, double *h) {
    char *end;
    double a = strtod(v, &end);
    const char *x = skip_space(end);
    double b;
    if (end == v || *x != 'x' || !(a > 0) || a - a != 0 || !parse_double(skip_space(x + 1), &b) ||
        !(b > 0)) {
        return false;
    }
    *w = a;
    *h = b;
    return true;
}

/* A number from 0 to 1. */
static bool parse_fraction(const char *v, double *out) {
    double d;
    if (!parse_double(v, &d) || d < 0 || d > 1) {
        return false;
    }
    *out = d;
    return true;
}

/* "10%" as 0.1, "none" as 0: a share of the hour above 0 and at most 100%. */
static bool parse_share(const char *v, double *out) {
    if (strcmp(v, "none") == 0) {
        *out = 0;
        return true;
    }
    char *end;
    double d = strtod(v, &end);
    if (end == v || *skip_space(end) != '%' || *skip_space(skip_space(end) + 1) != '\0' ||
        !(d > 0 && d <= 100)) {
        return false;
    }
    *out = d / 100;
    return true;
}

/* --- Plugins the format knows --- */

struct tsim_plugin {
    const char *name;
    const struct tsim_routing *routing; /* one of the two */
    const struct tsim_mac *mac;
    size_t config_size;
    void (*defaults)(void *config, const struct tsim_radio *radio);
    /* NULL if the setting took, or what was wrong with it. */
    const char *(*set)(void *config, const char *key, const char *value);
    /* Optional: NULL if the settings together make sense, or what is wrong with them. Run once
     * every setting is in, so settings that constrain each other can come in any order. */
    const char *(*check)(const void *config);
};

static void flood_defaults(void *config, const struct tsim_radio *radio) {
    *(struct tsim_flood_config *)config = (struct tsim_flood_config){
        .channel = radio->channel,
        .lora = radio->lora,
        .tx_dbm = radio->tx_dbm,
        .hops = 3,
    };
}

static const char *flood_set(void *config, const char *key, const char *value) {
    struct tsim_flood_config *c = config;
    uint64_t v;
    if (strcmp(key, "hops") == 0) {
        if (!parse_u64(value, UINT8_MAX, &v)) {
            return "expected a hop count from 0 to 255";
        }
        c->hops = (uint8_t)v;
        return NULL;
    }
    return "is not a setting of flood";
}

static void aloha_defaults(void *config, const struct tsim_radio *radio) {
    (void)radio;
    *(struct tsim_aloha_config *)config = (struct tsim_aloha_config){.max_delay = TSIM_S(1)};
}

static const char *aloha_set(void *config, const char *key, const char *value) {
    struct tsim_aloha_config *c = config;
    if (strcmp(key, "max_delay") == 0) {
        return parse_time(value, &c->max_delay) ? NULL : "expected a time, such as 2 s";
    }
    return "is not a setting of aloha";
}

static bool parse_yes_no(const char *v, bool *out) {
    if (strcmp(v, "yes") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(v, "no") == 0) {
        *out = false;
        return true;
    }
    return false;
}

/* Which nodes churn or movement takes: enum tsim_churn_nodes. */
static bool parse_kinds(const char *v, uint8_t *out) {
    static const char *names[] = {"all", "relays", "leaves"};
    for (uint8_t i = 0; i < sizeof names / sizeof *names; i++) {
        if (strcmp(v, names[i]) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

/* A contention window setting, shared by Meshtastic's routing and MAC. NULL if `key` is not one,
 * else "" if it took, or what was wrong with it. */
static const char *window_set(struct tsim_meshtastic_window *w, const char *key,
                              const char *value) {
    uint64_t v;
    if (strcmp(key, "slot") == 0) {
        return parse_time(value, &w->slot) ? "" : "expected a time, such as 40 ms";
    }
    uint8_t *cw = strcmp(key, "cw_min") == 0   ? &w->cw_min
                  : strcmp(key, "cw_max") == 0 ? &w->cw_max
                                               : NULL;
    if (!cw) {
        return NULL;
    }
    if (!parse_u64(value, 15, &v)) {
        return "expected a window exponent from 0 to 15";
    }
    *cw = (uint8_t)v;
    return "";
}

static const char *window_check(const struct tsim_meshtastic_window *w) {
    if (w->cw_min > w->cw_max) {
        return "cw_min is over cw_max";
    }
    if (w->slot == 0) {
        return "slot must be above 0";
    }
    if (!tsim_meshtastic_window_valid(w)) {
        return "slot is too long: 2^(cw_max + 1) slots must fit in a quarter of the clock";
    }
    return NULL;
}

static void meshtastic_defaults(void *config, const struct tsim_radio *radio) {
    *(struct tsim_meshtastic_config *)config =
        tsim_meshtastic_default(radio->channel, &radio->lora, radio->tx_dbm);
}

/* Where a routing's config keeps its relay pick - distvec's infrastructure, meshcore's repeaters,
 * meshtastic's routers - or all NULL for a routing without one. */
struct picked {
    uint8_t *pick; /* enum tsim_distvec_pick */
    uint32_t *count;
    const uint8_t **set;
};

static struct picked picked_of(const struct tsim_routing *routing, void *config) {
    if (routing == &tsim_distvec) {
        struct tsim_distvec_config *c = config;
        return (struct picked){&c->relay_pick, &c->relay_count, &c->relay_set};
    }
    if (routing == &tsim_meshcore) {
        struct tsim_meshcore_config *c = config;
        return (struct picked){&c->relay_pick, &c->relay_count, &c->relay_set};
    }
    if (routing == &tsim_meshtastic) {
        struct tsim_meshtastic_config *c = config;
        return (struct picked){&c->relay_pick, &c->relay_count, &c->relay_set};
    }
    return (struct picked){0};
}

/* routing.relay_pick and routing.relay_count, which distvec, meshcore and meshtastic share: NULL if
 * `key` is neither, "" if it took, or what was wrong with it. */
static const char *relay_pick_set(const char *key, const char *value, uint8_t *pick,
                                  uint32_t *count) {
    uint64_t v;
    if (strcmp(key, "relay_pick") == 0) {
        static const char *picks[] = {"list", "degree", "spaced", "cds"};
        for (uint8_t i = 0; i < sizeof picks / sizeof *picks; i++) {
            if (strcmp(value, picks[i]) == 0) {
                *pick = i;
                return "";
            }
        }
        return "expected list, degree, spaced or cds";
    }
    if (strcmp(key, "relay_count") == 0) {
        return parse_u64(value, UINT32_MAX, &v) ? (*count = (uint32_t)v, "") : "expected a count";
    }
    return NULL;
}

/* Why a relay pick would be refused, or NULL. */
static const char *relay_pick_check(uint8_t pick, uint32_t count) {
    if (pick > TSIM_DISTVEC_PICK_CDS ||
        (pick != TSIM_DISTVEC_PICK_LIST && pick != TSIM_DISTVEC_PICK_CDS && count == 0)) {
        return "relay_pick is not list, degree, spaced or cds, or relay_count is 0 for one but cds";
    }
    return NULL;
}

static const char *meshtastic_set(void *config, const char *key, const char *value) {
    struct tsim_meshtastic_config *c = config;
    uint64_t v;
    const char *why = window_set(&c->window, key, value);
    if (!why) {
        why = relay_pick_set(key, value, &c->relay_pick, &c->relay_count);
    }
    if (why) {
        return *why ? why : NULL;
    }
    if (strcmp(key, "role") == 0) {
        static const struct {
            const char *name;
            enum tsim_meshtastic_role role;
        } roles[] = {{"client", TSIM_MESHTASTIC_CLIENT},
                     {"client_mute", TSIM_MESHTASTIC_CLIENT_MUTE},
                     {"router", TSIM_MESHTASTIC_ROUTER}};
        for (size_t i = 0; i < sizeof roles / sizeof roles[0]; i++) {
            if (strcmp(value, roles[i].name) == 0) {
                c->role = roles[i].role;
                return NULL;
            }
        }
        return "expected client, client_mute or router";
    }
    if (strcmp(key, "hop_limit") == 0) {
        if (!parse_u64(value, TSIM_MESHTASTIC_HOPS_MAX, &v)) {
            return "expected a hop count from 0 to 7";
        }
        c->hop_limit = (uint8_t)v;
        return NULL;
    }
    if (strcmp(key, "want_ack") == 0) {
        return parse_yes_no(value, &c->want_ack) ? NULL : "expected yes or no";
    }
    if (strcmp(key, "ack_duplicates") == 0) {
        return parse_yes_no(value, &c->ack_duplicates) ? NULL : "expected yes or no";
    }
    if (strcmp(key, "ack_poll") == 0) {
        return parse_yes_no(value, &c->ack_poll) ? NULL : "expected yes or no";
    }
    if (strcmp(key, "cancel_late") == 0) {
        return parse_yes_no(value, &c->cancel_late) ? NULL : "expected yes or no";
    }
    if (strcmp(key, "retries") == 0) {
        if (!parse_u64(value, UINT8_MAX, &v)) {
            return "expected a count from 0 to 255";
        }
        c->retries = (uint8_t)v;
        return NULL;
    }
    if (strcmp(key, "noise") == 0) {
        return parse_double(value, &c->noise_dbm) && fabs(c->noise_dbm) <= 1e3
                   ? NULL
                   : "expected dBm from -1000 to 1000";
    }
    if (strcmp(key, "processing") == 0) {
        if (!parse_time(value, &c->processing)) {
            return "expected a time, such as 4.5 s";
        }
        return c->processing <= TSIM_MESHTASTIC_WAIT_MAX ? NULL : "is too long for the clock";
    }
    if (strcmp(key, "background") == 0) {
        return parse_yes_no(value, &c->background) ? NULL : "expected yes or no";
    }
    tsim_time *every = strcmp(key, "nodeinfo_interval") == 0    ? &c->nodeinfo_interval
                       : strcmp(key, "position_interval") == 0  ? &c->position_interval
                       : strcmp(key, "telemetry_interval") == 0 ? &c->telemetry_interval
                                                                : NULL;
    if (every) {
        if (strcmp(value, "default") == 0) {
            *every = 0;
            return NULL;
        }
        return parse_time(value, every) && *every > 0 ? NULL
                                                      : "expected a time above 0, or default";
    }
    if (strcmp(key, "position_share") == 0) {
        return parse_fraction(value, &c->position_share) ? NULL : "expected a fraction from 0 to 1";
    }
    if (strcmp(key, "nodedb_max") == 0) {
        if (!parse_u64(value, 250, &v) || v < 2) {
            return "expected a count from 2 to 250";
        }
        c->nodedb_max = (uint16_t)v;
        return NULL;
    }
    if (strcmp(key, "throttle") == 0) {
        return parse_fraction(value, &c->throttle) ? NULL : "expected a fraction from 0 to 1";
    }
    if (strcmp(key, "duty_cycle") == 0) {
        double share;
        if (!parse_share(value, &share)) {
            return "expected a percentage such as 10%, or none";
        }
        c->duty_cycle = share * 100;
        return NULL;
    }
    return "is not a setting of meshtastic";
}

static const char *meshtastic_check(const void *config) {
    const struct tsim_meshtastic_config *c = config;
    const char *why = relay_pick_check(c->relay_pick, c->relay_count);
    if (!why && c->background && c->cancel_late) {
        why = "background and cancel_late cannot be used together";
    }
    return why ? why : window_check(&c->window);
}

static void meshtastic_mac_defaults(void *config, const struct tsim_radio *radio) {
    *(struct tsim_meshtastic_mac_config *)config = tsim_meshtastic_mac_default(&radio->lora);
}

static const char *meshtastic_mac_set(void *config, const char *key, const char *value) {
    struct tsim_meshtastic_mac_config *c = config;
    const char *why = window_set(&c->window, key, value);
    if (why) {
        return *why ? why : NULL;
    }
    double *db = strcmp(key, "snr_min") == 0   ? &c->snr_min_db
                 : strcmp(key, "snr_max") == 0 ? &c->snr_max_db
                                               : NULL;
    if (db) {
        return parse_double(value, db) ? NULL : "expected an SNR in dB";
    }
    if (strcmp(key, "busy_chance") == 0) {
        return parse_fraction(value, &c->busy_chance) ? NULL : "expected a fraction from 0 to 1";
    }
    return "is not a setting of meshtastic";
}

static const char *meshtastic_mac_check(const void *config) {
    const struct tsim_meshtastic_mac_config *c = config;
    if (!(c->snr_min_db < c->snr_max_db)) {
        return "snr_min is not below snr_max";
    }
    return window_check(&c->window);
}

static const char *meshcore_check(const void *config) {
    const struct tsim_meshcore_config *c = config;
    return relay_pick_check(c->relay_pick, c->relay_count);
}

static void meshcore_defaults(void *config, const struct tsim_radio *radio) {
    *(struct tsim_meshcore_config *)config =
        tsim_meshcore_default(radio->channel, &radio->lora, radio->tx_dbm);
}

/* A factor of airtime from 0 to 2, as the firmware's command line allows. */
static const char *delay_factor(const char *value, double *out) {
    return parse_double(value, out) && *out >= 0 && *out <= 2 ? NULL
                                                              : "expected a factor from 0 to 2";
}

static const char *meshcore_set(void *config, const char *key, const char *value) {
    struct tsim_meshcore_config *c = config;
    uint64_t v;
    const char *why = relay_pick_set(key, value, &c->relay_pick, &c->relay_count);
    if (why) {
        return *why ? why : NULL;
    }
    if (strcmp(key, "relays") == 0) {
        if (!tsim_meshcore_relays_valid(value)) {
            return "expected all, or node numbers and ranges such as 0-45,50";
        }
        strcpy(c->relays, value);
        return NULL;
    }
    if (strcmp(key, "hash_size") == 0) {
        if (!parse_u64(value, 3, &v) || v == 0) {
            return "expected 1, 2 or 3 bytes";
        }
        c->hash_size = (uint8_t)v;
        return NULL;
    }
    if (strcmp(key, "scoped") == 0) {
        return parse_yes_no(value, &c->scoped) ? NULL : "expected yes or no";
    }
    if (strcmp(key, "flood_max") == 0) {
        if (!parse_u64(value, 64, &v) || v == 0) {
            return "expected a hop count from 1 to 64";
        }
        c->flood_max = (uint8_t)v;
        return NULL;
    }
    if (strcmp(key, "rx_delay_base") == 0) {
        return parse_double(value, &c->rx_delay_base) && c->rx_delay_base >= 0 &&
                       c->rx_delay_base <= 20
                   ? NULL
                   : "expected a base from 0 to 20";
    }
    if (strcmp(key, "tx_delay_factor") == 0) {
        return delay_factor(value, &c->tx_delay_factor);
    }
    if (strcmp(key, "direct_tx_delay_factor") == 0) {
        return delay_factor(value, &c->direct_tx_delay_factor);
    }
    if (strcmp(key, "retries") == 0) {
        if (!parse_u64(value, UINT8_MAX, &v)) {
            return "expected a count from 0 to 255";
        }
        c->retries = (uint8_t)v;
        return NULL;
    }
    if (strcmp(key, "advert_interval") == 0) {
        if (strcmp(value, "none") == 0) {
            c->advert_interval = 0;
            return NULL;
        }
        return parse_time(value, &c->advert_interval) && c->advert_interval > 0
                   ? NULL
                   : "expected a time above 0, or none";
    }
    if (strcmp(key, "flood_max_advert") == 0) {
        if (!parse_u64(value, 64, &v) || v == 0) {
            return "expected a hop count from 1 to 64";
        }
        c->flood_max_advert = (uint8_t)v;
        return NULL;
    }
    if (strcmp(key, "background") == 0) {
        return parse_yes_no(value, &c->background) ? NULL : "expected yes or no";
    }
    tsim_time *every = strcmp(key, "flood_advert_interval") == 0 ? &c->flood_advert_interval
                       : strcmp(key, "companion_advert_interval") == 0
                           ? &c->companion_advert_interval
                           : NULL;
    if (every) {
        if (strcmp(value, "none") == 0) {
            *every = 0;
            return NULL;
        }
        return parse_time(value, every) && *every > 0 ? NULL : "expected a time above 0, or none";
    }
    if (strcmp(key, "companion_advert_flood") == 0) {
        return parse_yes_no(value, &c->companion_advert_flood) ? NULL : "expected yes or no";
    }
    if (strcmp(key, "cancel_heard") == 0) {
        static const char *const names[] = {"no", "waiting", "queued"};
        for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
            if (strcmp(value, names[i]) == 0) {
                c->cancel_heard = (enum tsim_meshcore_cancel)i;
                return NULL;
            }
        }
        return "expected no, waiting or queued";
    }
    if (strcmp(key, "estimate_cr") == 0) {
        if (strcmp(value, "radio") == 0) {
            c->estimate_cr = 0;
            return NULL;
        }
        if (!parse_u64(value, 4, &v) || v == 0) {
            return "expected radio, or a coding rate from 1 to 4";
        }
        c->estimate_cr = (uint8_t)v;
        return NULL;
    }
    return "is not a setting of meshcore";
}

static void meshcore_mac_defaults(void *config, const struct tsim_radio *radio) {
    (void)radio;
    *(struct tsim_meshcore_mac_config *)config = tsim_meshcore_mac_default();
}

static const char *meshcore_mac_set(void *config, const char *key, const char *value) {
    struct tsim_meshcore_mac_config *c = config;
    if (strcmp(key, "airtime_factor") == 0) {
        return parse_double(value, &c->airtime_factor) && c->airtime_factor >= 0 &&
                       c->airtime_factor <= 1e6
                   ? NULL
                   : "expected a factor from 0 to 1000000";
    }
    if (strcmp(key, "latched_header") == 0) {
        if (strcmp(value, "no") == 0) {
            c->latched_header = 0;
            return NULL;
        }
        return parse_time(value, &c->latched_header) && c->latched_header > 0
                   ? NULL
                   : "expected a time above 0, or no";
    }
    return "is not a setting of meshcore";
}

static void distvec_defaults(void *config, const struct tsim_radio *radio) {
    *(struct tsim_distvec_config *)config =
        tsim_distvec_default(radio->channel, &radio->lora, radio->tx_dbm);
}

static const char *distvec_count(const char *value, uint64_t lo, uint64_t hi, uint8_t *out) {
    uint64_t v;
    if (!parse_u64(value, hi, &v) || v < lo) {
        return lo ? "expected a count from 1 to its limit" : "expected a count from 0 to its limit";
    }
    *out = (uint8_t)v;
    return NULL;
}

static const char *distvec_factor(const char *value, double lo, double hi, double *out) {
    return parse_double(value, out) && *out >= lo && *out <= hi ? NULL
                                                                : "expected a number in range";
}

static const char *distvec_time(const char *value, bool zero, tsim_time *out) {
    return parse_time(value, out) && (zero || *out > 0) ? NULL : "expected a time, such as 2 s";
}

static const char *distvec_set(void *config, const char *key, const char *value) {
    struct tsim_distvec_config *c = config;
    uint64_t v;
    if (strcmp(key, "relays") == 0) {
        if (!tsim_meshcore_relays_valid(value)) {
            return "expected all, or node numbers and ranges such as 0-45,50";
        }
        strcpy(c->relays, value);
        return NULL;
    }
    if (strcmp(key, "relay_pick") == 0 && strcmp(value, "elect") == 0) {
        c->relay_pick = TSIM_DISTVEC_PICK_ELECT;
        return NULL;
    }
    const char *why = relay_pick_set(key, value, &c->relay_pick, &c->relay_count);
    if (why) {
        return *why ? why : NULL;
    }
    if (strcmp(key, "elect_cover") == 0) {
        return distvec_count(value, 1, 8, &c->elect_cover);
    }
    if (strcmp(key, "elect_wait") == 0) {
        return distvec_time(value, false, &c->elect_wait);
    }
    if (strcmp(key, "elect_hold") == 0) {
        return distvec_time(value, true, &c->elect_hold);
    }
    if (strcmp(key, "leaves") == 0) {
        if (strcmp(value, "routed") == 0) {
            c->leaves = TSIM_DISTVEC_LEAVES_ROUTED;
        } else if (strcmp(value, "parent_oracle") == 0) {
            c->leaves = TSIM_DISTVEC_LEAVES_PARENT_ORACLE;
        } else if (strcmp(value, "parent") == 0) {
            c->leaves = TSIM_DISTVEC_LEAVES_PARENT;
        } else {
            return "expected routed, parent_oracle or parent";
        }
        return NULL;
    }
    if (strcmp(key, "routes") == 0) {
        if (strcmp(value, "proactive") == 0) {
            c->routes = TSIM_DISTVEC_ROUTES_PROACTIVE;
        } else if (strcmp(value, "demand") == 0) {
            c->routes = TSIM_DISTVEC_ROUTES_DEMAND;
        } else {
            return "expected proactive or demand";
        }
        return NULL;
    }
    if (strcmp(key, "route_ttl") == 0) {
        return distvec_time(value, false, &c->route_ttl);
    }
    if (strcmp(key, "req_hops") == 0) {
        return distvec_count(value, 1, UINT8_MAX, &c->req_hops);
    }
    if (strcmp(key, "req_cancel") == 0) {
        return distvec_count(value, 0, UINT8_MAX, &c->req_cancel);
    }
    if (strcmp(key, "imin") == 0) {
        return distvec_time(value, false, &c->imin);
    }
    if (strcmp(key, "doublings") == 0) {
        return distvec_count(value, 0, 16, &c->doublings);
    }
    if (strcmp(key, "redundancy") == 0) {
        return distvec_count(value, 0, UINT8_MAX, &c->redundancy);
    }
    if (strcmp(key, "quiet_max") == 0) {
        return distvec_count(value, 0, UINT8_MAX, &c->quiet_max);
    }
    if (strcmp(key, "neighbour_timeout") == 0) {
        return distvec_time(value, false, &c->neighbour_timeout);
    }
    if (strcmp(key, "cap") == 0) {
        return parse_double(value, &c->cap) && c->cap > 0 && c->cap <= 1
                   ? NULL
                   : "expected a share above 0 and at most 1";
    }
    if (strcmp(key, "request_share") == 0) {
        return parse_double(value, &c->request_share) && c->request_share > 0 &&
                       c->request_share < 1
                   ? NULL
                   : "expected a share above 0 and below 1";
    }
    if (strcmp(key, "cap_window") == 0) {
        return distvec_time(value, false, &c->cap_window);
    }
    if (strcmp(key, "burst") == 0) {
        return distvec_count(value, 1, 16, &c->burst);
    }
    if (strcmp(key, "ihu_max") == 0) {
        return distvec_count(value, 0, 48, &c->ihu_max);
    }
    if (strcmp(key, "ihu_rounds") == 0) {
        return distvec_count(value, 2, 64, &c->ihu_rounds);
    }
    if (strcmp(key, "ref_len") == 0) {
        if (!parse_u64(value, 255, &v)) {
            return "expected a length from 0 to 255 bytes";
        }
        c->ref_len = (uint32_t)v;
        return NULL;
    }
    if (strcmp(key, "etx") == 0) {
        return parse_yes_no(value, &c->etx) ? NULL : "expected yes or no";
    }
    if (strcmp(key, "etx_max") == 0) {
        return distvec_factor(value, 1, 1e6, &c->etx_max);
    }
    if (strcmp(key, "hysteresis") == 0) {
        return distvec_factor(value, 0, 1, &c->hysteresis);
    }
    if (strcmp(key, "change") == 0) {
        return distvec_factor(value, 0, 1, &c->change);
    }
    if (strcmp(key, "request_interval") == 0) {
        return distvec_time(value, true, &c->request_interval);
    }
    if (strcmp(key, "seq_period") == 0) {
        return distvec_time(value, true, &c->seq_period);
    }
    if (strcmp(key, "hop_max") == 0) {
        return distvec_count(value, 1, UINT8_MAX, &c->hop_max);
    }
    if (strcmp(key, "hop_retries") == 0) {
        return distvec_count(value, 0, UINT8_MAX, &c->hop_retries);
    }
    if (strcmp(key, "hop_wait") == 0) {
        return distvec_time(value, true, &c->hop_wait);
    }
    if (strcmp(key, "retries") == 0) {
        return distvec_count(value, 0, UINT8_MAX, &c->retries);
    }
    if (strcmp(key, "ack_wait") == 0) {
        return distvec_time(value, false, &c->ack_wait);
    }
    if (strcmp(key, "ack_factor") == 0) {
        return distvec_factor(value, 0, 1e3, &c->ack_factor);
    }
    if (strcmp(key, "jitter") == 0) {
        return distvec_factor(value, 0, 1e3, &c->jitter);
    }
    if (strcmp(key, "bcast_hops") == 0) {
        return distvec_count(value, 0, 254, &c->bcast_hops);
    }
    if (strcmp(key, "bcast_window") == 0) {
        return distvec_factor(value, 0, 1e3, &c->bcast_window);
    }
    if (strcmp(key, "bcast_cancel") == 0) {
        return distvec_count(value, 0, UINT8_MAX, &c->bcast_cancel);
    }
    if (strcmp(key, "power") == 0) {
        return parse_yes_no(value, &c->power) ? NULL : "expected yes or no";
    }
    if (strcmp(key, "tx_min") == 0) {
        return parse_double(value, &c->tx_min_dbm) ? NULL : "expected a power in dBm";
    }
    if (strcmp(key, "margin") == 0) {
        return distvec_factor(value, 0, 60, &c->margin_db);
    }
    if (strcmp(key, "step") == 0) {
        return distvec_factor(value, 0, 60, &c->step_db);
    }
    if (strcmp(key, "power_k") == 0) {
        return distvec_count(value, 0, UINT8_MAX, &c->power_k);
    }
    if (strcmp(key, "sf_min") == 0) {
        return distvec_count(value, 0, 12, &c->sf_min);
    }
    if (strcmp(key, "sf_k") == 0) {
        return distvec_count(value, 1, UINT8_MAX, &c->sf_k);
    }
    if (strcmp(key, "snr_floor") == 0) {
        return parse_double(value, &c->snr_floor_db) ? NULL : "expected an SNR in dB";
    }
    if (strcmp(key, "oracle") == 0) {
        return parse_yes_no(value, &c->oracle) ? NULL : "expected yes or no";
    }
    if (strcmp(key, "links") == 0) {
        if (strcmp(value, "sensed") == 0) {
            c->links = TSIM_DISTVEC_LINKS_SENSED;
        } else if (strcmp(value, "oracle") == 0) {
            c->links = TSIM_DISTVEC_LINKS_ORACLE;
        } else if (strcmp(value, "strength") == 0) {
            c->links = TSIM_DISTVEC_LINKS_STRENGTH;
        } else {
            return "expected sensed, oracle or strength";
        }
        return NULL;
    }
    if (strcmp(key, "bcast_power") == 0) {
        static const char *const names[] = {"k", "relays", "full", "routes"};
        for (uint8_t i = 0; i < sizeof names / sizeof names[0]; i++) {
            if (strcmp(value, names[i]) == 0) {
                c->bcast_power = i;
                return NULL;
            }
        }
        return "expected k, relays, full or routes";
    }
    if (strcmp(key, "bcast_k") == 0) {
        return distvec_count(value, 0, UINT8_MAX, &c->bcast_k);
    }
    if (strcmp(key, "link_margin") == 0) {
        return distvec_factor(value, 0, 60, &c->link_margin_db);
    }
    if (strcmp(key, "link_band") == 0) {
        return distvec_factor(value, 0, 60, &c->link_band_db);
    }
    if (strcmp(key, "dead_hops") == 0) {
        return distvec_count(value, 1, UINT8_MAX, &c->dead_hops);
    }
    if (strcmp(key, "silent_max") == 0) {
        return distvec_time(value, false, &c->silent_max);
    }
    if (strcmp(key, "probe_hops") == 0) {
        return distvec_count(value, 0, UINT8_MAX, &c->probe_hops);
    }
    if (strcmp(key, "probe_tries") == 0) {
        return distvec_count(value, 1, 32, &c->probe_tries);
    }
    if (strcmp(key, "probe_wait") == 0) {
        return distvec_time(value, false, &c->probe_wait);
    }
    if (strcmp(key, "reattach") == 0) {
        return parse_yes_no(value, &c->reattach) ? NULL : "expected yes or no";
    }
    if (strcmp(key, "solicit_quiet") == 0) {
        return distvec_time(value, true, &c->solicit_quiet);
    }
    if (strcmp(key, "solicit_wait") == 0) {
        return distvec_time(value, false, &c->solicit_wait);
    }
    if (strcmp(key, "solicit_gap") == 0) {
        return distvec_time(value, false, &c->solicit_gap);
    }
    if (strcmp(key, "solicit_tries") == 0) {
        return distvec_count(value, 1, UINT8_MAX, &c->solicit_tries);
    }
    if (strcmp(key, "here_window") == 0) {
        return distvec_factor(value, 0, 1e3, &c->here_window);
    }
    if (strcmp(key, "solicit_hops") == 0) {
        return distvec_count(value, 1, UINT8_MAX, &c->solicit_hops);
    }
    if (strcmp(key, "leaf_tries") == 0) {
        return distvec_count(value, 0, UINT8_MAX - 1, &c->leaf_tries);
    }
    if (strcmp(key, "here_cancel") == 0) {
        return distvec_count(value, 0, UINT8_MAX, &c->here_cancel);
    }
    if (strcmp(key, "oracle_margin") == 0) {
        return distvec_factor(value, 0, 60, &c->oracle_margin_db);
    }
    return "is not a setting of distvec";
}

static const char *distvec_check(const void *config) { return tsim_distvec_check(config); }

_Static_assert(sizeof(struct tsim_flood_config) <= TSIM_PLUGIN_CONFIG_MAX, "flood config");
_Static_assert(sizeof(struct tsim_aloha_config) <= TSIM_PLUGIN_CONFIG_MAX, "aloha config");
_Static_assert(sizeof(struct tsim_meshtastic_config) <= TSIM_PLUGIN_CONFIG_MAX,
               "meshtastic config");
_Static_assert(sizeof(struct tsim_meshtastic_mac_config) <= TSIM_PLUGIN_CONFIG_MAX,
               "meshtastic mac config");
_Static_assert(sizeof(struct tsim_meshcore_config) <= TSIM_PLUGIN_CONFIG_MAX, "meshcore config");
_Static_assert(sizeof(struct tsim_meshcore_mac_config) <= TSIM_PLUGIN_CONFIG_MAX,
               "meshcore mac config");
_Static_assert(sizeof(struct tsim_distvec_config) <= TSIM_PLUGIN_CONFIG_MAX, "distvec config");

static const struct tsim_plugin plugins[] = {
    {"flood", &tsim_flood, NULL, sizeof(struct tsim_flood_config), flood_defaults, flood_set, NULL},
    {"aloha", NULL, &tsim_aloha, sizeof(struct tsim_aloha_config), aloha_defaults, aloha_set, NULL},
    {"meshtastic", &tsim_meshtastic, NULL, sizeof(struct tsim_meshtastic_config),
     meshtastic_defaults, meshtastic_set, meshtastic_check},
    {"meshtastic", NULL, &tsim_meshtastic_mac, sizeof(struct tsim_meshtastic_mac_config),
     meshtastic_mac_defaults, meshtastic_mac_set, meshtastic_mac_check},
    {"meshcore", &tsim_meshcore, NULL, sizeof(struct tsim_meshcore_config), meshcore_defaults,
     meshcore_set, meshcore_check},
    {"meshcore", NULL, &tsim_meshcore_mac, sizeof(struct tsim_meshcore_mac_config),
     meshcore_mac_defaults, meshcore_mac_set, NULL},
    {"distvec", &tsim_distvec, NULL, sizeof(struct tsim_distvec_config), distvec_defaults,
     distvec_set, distvec_check},
};

static const struct tsim_plugin *find_plugin(const char *name, bool routing) {
    for (size_t i = 0; i < sizeof plugins / sizeof plugins[0]; i++) {
        if (strcmp(plugins[i].name, name) == 0 && (plugins[i].routing != NULL) == routing) {
            return &plugins[i];
        }
    }
    return NULL;
}

const char *tsim_scenario_routing_name(const struct tsim_scenario *s) { return s->routing->name; }

const char *tsim_scenario_mac_name(const struct tsim_scenario *s) { return s->mac->name; }

/* --- Settings --- */

static void defaults(struct tsim_scenario *s) {
    *s = (struct tsim_scenario){
        .seed = 1,
        .placement = TSIM_PLACEMENT_UNIFORM,
        .width_m = 5000,
        .height_m = 5000,
        .spacing_m = 1000,
        .channel = tsim_channel_default(1),
        .net = tsim_net_defaults(1),
        .radio = {.channel = 0, .lora = tsim_lora_default(7, 125000), .tx_dbm = 14.0},
        .interval = TSIM_S(15 * 60),
        .len_min = 32,
        .len_max = 32,
        .broadcast = 1.0,
        .reply_delay = TSIM_S(2 * 60),
        .churn_up = TSIM_S(2 * 3600),
        .churn_down = TSIM_S(15 * 60),
        .move_nodes = TSIM_CHURN_LEAVES,
        .move_speed_min = 0.5,
        .move_speed_max = 2.0,
        .move_pause = TSIM_S(5 * 60),
        .move_step = TSIM_S(10),
        .warmup = 0,
        .duration = TSIM_S(3600),
        .deadline = TSIM_S(60),
    };
}

static char *trim(char *s);

/* "30 s, 2, all, 40": when, from which node, to which or to all, and how many bytes. Each adds a
 * message; whether its nodes exist is checked once nodes is known. */
static const char *parse_send(struct tsim_scenario *s, const char *v) {
    static const char *const why = "expected a time, a node, a node or all, and bytes up to 255, "
                                   "as 30 s, 2, all, 40";
    char buf[128];
    if (strlen(v) >= sizeof buf) {
        return why;
    }
    strcpy(buf, v);
    char *field[4];
    char *at = buf;
    for (int i = 0; i < 4; i++) {
        char *comma = strchr(at, ',');
        if ((i < 3) != (comma != NULL)) {
            return why;
        }
        if (comma) {
            *comma = '\0';
        }
        field[i] = trim(at);
        at = comma ? comma + 1 : at;
    }
    struct tsim_send send;
    uint64_t src, dst = 0, len;
    bool all = strcmp(field[2], "all") == 0;
    if (!parse_time(field[0], &send.at) || !parse_u64(field[1], UINT32_MAX - 1, &src) ||
        !(all || parse_u64(field[2], UINT32_MAX - 1, &dst)) ||
        !parse_u64(field[3], TSIM_FRAME_MAX, &len)) {
        return why;
    }
    if (s->send_count == TSIM_SENDS_MAX) {
        return "is one more send than the 64 a scenario can hold";
    }
    send.src = (uint32_t)src;
    send.dst = all ? TSIM_BROADCAST : (uint32_t)dst;
    send.len = (uint32_t)len;
    s->sends[s->send_count++] = send;
    return NULL;
}

/* NULL if the setting took, or what was wrong with it. */
static const char *set_core(struct tsim_scenario *s, const char *key, const char *v) {
    uint64_t u;
    if (strcmp(key, "seed") == 0) {
        return parse_u64(v, UINT64_MAX, &s->seed) ? NULL : "expected a whole number";
    }
    if (strcmp(key, "nodes") == 0) {
        if (!parse_u64(v, 1u << 20, &u) || u == 0) {
            return "expected a number of nodes from 1 to 1048576";
        }
        s->nodes = (uint32_t)u;
        return NULL;
    }
    if (strcmp(key, "placement") == 0) {
        if (strcmp(v, "uniform") == 0) {
            s->placement = TSIM_PLACEMENT_UNIFORM;
        } else if (strcmp(v, "grid") == 0) {
            s->placement = TSIM_PLACEMENT_GRID;
        } else if (strcmp(v, "line") == 0) {
            s->placement = TSIM_PLACEMENT_LINE;
        } else if (strcmp(v, "file") == 0) {
            s->placement = TSIM_PLACEMENT_FILE;
        } else {
            return "expected uniform, grid, line or file";
        }
        return NULL;
    }
    if (strcmp(key, "positions") == 0) {
        if (strlen(v) >= sizeof s->positions_file) {
            return "is too long a path";
        }
        strcpy(s->positions_file, v);
        return NULL;
    }
    if (strcmp(key, "links") == 0) {
        if (strlen(v) >= sizeof s->links_file) {
            return "is too long a path";
        }
        strcpy(s->links_file, v);
        return NULL;
    }
    if (strcmp(key, "area") == 0) {
        return parse_area(v, &s->width_m, &s->height_m) ? NULL
                                                        : "expected metres, such as 5000 x 5000";
    }
    if (strcmp(key, "spacing") == 0) {
        return parse_double(v, &s->spacing_m) && s->spacing_m > 0 ? NULL
                                                                  : "expected metres above 0";
    }
    if (strcmp(key, "routing") == 0 || strcmp(key, "mac") == 0) {
        bool routing = key[0] == 'r';
        const struct tsim_plugin *p = find_plugin(v, routing);
        if (!p) {
            return routing ? "is not a routing this build knows" : "is not a MAC this build knows";
        }
        *(routing ? &s->routing : &s->mac) = p;
        return NULL;
    }
    if (strcmp(key, "warmup") == 0) {
        return parse_time(v, &s->warmup) ? NULL : "expected a time, such as 5 min";
    }
    if (strcmp(key, "duration") == 0) {
        return parse_time(v, &s->duration) && s->duration > 0 ? NULL : "expected a time above 0";
    }
    if (strcmp(key, "deadline") == 0) {
        return parse_time(v, &s->deadline) ? NULL : "expected a time, such as 30 s";
    }
    if (strcmp(key, "queue") == 0) {
        if (!parse_u64(v, UINT32_MAX, &u)) {
            return "expected a number of frames, 0 for no limit";
        }
        s->net.queue_limit = (uint32_t)u;
        return NULL;
    }
    if (strcmp(key, "duty_cycle") == 0) {
        return parse_share(v, &s->net.duty_cycle) ? NULL
                                                  : "expected a percentage such as 10%, or none";
    }

    if (strcmp(key, "radio.channel") == 0) {
        if (!parse_u64(v, UINT16_MAX, &u)) {
            return "expected a channel number";
        }
        s->radio.channel = (uint16_t)u;
        return NULL;
    }
    if (strcmp(key, "radio.sf") == 0) {
        if (!parse_u64(v, 12, &u) || u < 7) {
            return "expected a spreading factor from 7 to 12";
        }
        s->radio.lora.sf = (uint8_t)u;
        return NULL;
    }
    if (strcmp(key, "radio.bw") == 0) {
        if (!parse_u64(v, 500000, &u) || u == 0) {
            return "expected a bandwidth in Hz, such as 125000";
        }
        s->radio.lora.bw_hz = (uint32_t)u;
        return NULL;
    }
    if (strcmp(key, "sites.sf") == 0) {
        if (!parse_u64(v, 12, &u) || (u != 0 && u < 7)) {
            return "expected 0 for the radio's, or a spreading factor from 7 to 12";
        }
        s->sites_sf = (uint8_t)u;
        return NULL;
    }
    if (strcmp(key, "sites.bw") == 0) {
        if (!parse_u64(v, 500000, &u)) {
            return "expected 0 for the radio's, or a bandwidth in Hz, such as 125000";
        }
        s->sites_bw = (uint32_t)u;
        return NULL;
    }
    if (strcmp(key, "radio.cr") == 0) {
        if (!parse_u64(v, 4, &u) || u == 0) {
            return "expected a coding rate from 1 (4/5) to 4 (4/8)";
        }
        s->radio.lora.cr = (uint8_t)u;
        return NULL;
    }
    if (strcmp(key, "radio.preamble") == 0) {
        if (!parse_u64(v, UINT16_MAX, &u)) {
            return "expected a preamble length in symbols";
        }
        s->radio.lora.preamble = (uint16_t)u;
        return NULL;
    }
    if (strcmp(key, "radio.tx_dbm") == 0) {
        return parse_double(v, &s->radio.tx_dbm) ? NULL : "expected a power in dBm";
    }

    if (strcmp(key, "channel.pl0") == 0) {
        return parse_double(v, &s->channel.pl0_db) ? NULL : "expected a loss in dB";
    }
    if (strcmp(key, "channel.d0") == 0) {
        return parse_double(v, &s->channel.d0_m) && s->channel.d0_m > 0 ? NULL
                                                                        : "expected metres above 0";
    }
    if (strcmp(key, "channel.exponent") == 0) {
        return parse_double(v, &s->channel.exponent) ? NULL : "expected a path loss exponent";
    }
    if (strcmp(key, "channel.sigma") == 0) {
        return parse_double(v, &s->channel.sigma_db) && s->channel.sigma_db >= 0
                   ? NULL
                   : "expected a standard deviation in dB, 0 or more";
    }
    if (strcmp(key, "channel.share") == 0) {
        return parse_fraction(v, &s->channel.node_share) ? NULL : "expected a fraction from 0 to 1";
    }
    if (strcmp(key, "channel.model") == 0) {
        static const struct {
            const char *name;
            enum tsim_path_model model;
        } models[] = {{"log_distance", TSIM_PATH_LOG_DISTANCE},
                      {"3gpp_suburban", TSIM_PATH_3GPP_SUBURBAN},
                      {"3gpp_urban", TSIM_PATH_3GPP_URBAN}};
        for (size_t i = 0; i < sizeof models / sizeof models[0]; i++) {
            if (strcmp(v, models[i].name) == 0) {
                s->channel.model = models[i].model;
                return NULL;
            }
        }
        return "expected log_distance, 3gpp_suburban or 3gpp_urban";
    }
    if (strcmp(key, "channel.freq") == 0) {
        return parse_double(v, &s->channel.freq_mhz) && s->channel.freq_mhz > 0
                   ? NULL
                   : "expected MHz above 0";
    }
    if (strcmp(key, "channel.height") == 0) {
        return parse_double(v, &s->channel.height_m) && s->channel.height_m > 0
                   ? NULL
                   : "expected metres above 0";
    }
    if (strcmp(key, "channel.decorrelation") == 0) {
        return parse_double(v, &s->channel.decorrelation_m) && s->channel.decorrelation_m > 0
                   ? NULL
                   : "expected metres above 0";
    }

    if (strcmp(key, "phy.noise_figure") == 0) {
        return parse_double(v, &s->net.phy.noise_figure_db) ? NULL : "expected dB";
    }
    if (strcmp(key, "phy.capture") == 0) {
        return parse_double(v, &s->net.phy.capture_db) ? NULL : "expected dB";
    }
    if (strcmp(key, "phy.lock_symbols") == 0) {
        if (!parse_u64(v, UINT16_MAX, &u)) {
            return "expected a number of symbols";
        }
        s->net.phy.lock_symbols = (uint16_t)u;
        return NULL;
    }
    if (strcmp(key, "phy.retune") == 0) {
        return parse_time(v, &s->net.phy.retune) ? NULL : "expected a time, such as 1 ms";
    }
    if (strcmp(key, "phy.fading") == 0) {
        double *f = &s->net.phy.fading_db;
        return parse_double(v, f) && *f >= 0 && *f <= 1e3 ? NULL : "expected dB from 0 to 1000";
    }
    if (strcmp(key, "phy.pairwise") == 0) {
        return parse_yes_no(v, &s->net.phy.pairwise) ? NULL : "expected yes or no";
    }
    if (strcmp(key, "phy.capture_anytime") == 0) {
        return parse_yes_no(v, &s->net.phy.capture_anytime) ? NULL : "expected yes or no";
    }
    if (strcmp(key, "phy.cad_margin") == 0) {
        double *m = &s->net.phy.cad_margin_db;
        return parse_double(v, m) && fabs(*m) <= 1e3 ? NULL : "expected dB from -1000 to 1000";
    }
    if (strcmp(key, "phy.cad_delay") == 0) {
        return parse_time(v, &s->net.phy.cad_delay) ? NULL : "expected a time, such as 30 ms";
    }

    if (strcmp(key, "traffic.interval") == 0) {
        if (strcmp(v, "none") == 0) {
            s->interval = 0;
            return NULL;
        }
        return parse_time(v, &s->interval) && s->interval > 0 ? NULL
                                                              : "expected a time above 0, or none";
    }
    if (strcmp(key, "traffic.send") == 0) {
        return parse_send(s, v);
    }
    if (strcmp(key, "traffic.len") == 0) {
        return parse_range(v, TSIM_FRAME_MAX, &s->len_min, &s->len_max)
                   ? NULL
                   : "expected bytes up to 255, as 32 or 16..64";
    }
    if (strcmp(key, "traffic.broadcast") == 0) {
        return parse_fraction(v, &s->broadcast) ? NULL : "expected a fraction from 0 to 1";
    }
    if (strcmp(key, "traffic.closed") == 0) {
        return parse_yes_no(v, &s->closed) ? NULL : "expected yes or no";
    }
    if (strcmp(key, "traffic.peers") == 0) {
        uint64_t peers;
        if (!parse_u64(v, UINT32_MAX - 1, &peers)) {
            return "expected a count of nodes, 0 for anyone";
        }
        s->peers = (uint32_t)peers;
        return NULL;
    }
    if (strcmp(key, "traffic.reply") == 0) {
        return parse_fraction(v, &s->reply) ? NULL : "expected a fraction from 0 to 1";
    }
    if (strcmp(key, "traffic.reply_delay") == 0) {
        return parse_time(v, &s->reply_delay) ? NULL : "expected a time, such as 2 min";
    }
    if (strcmp(key, "traffic.lead") == 0) {
        return parse_time(v, &s->lead) ? NULL : "expected a time, such as 3 h";
    }
    if (strcmp(key, "report.announces") == 0) {
        return parse_yes_no(v, &s->report_announces) ? NULL : "expected yes or no";
    }
    if (strcmp(key, "churn.share") == 0) {
        return parse_fraction(v, &s->churn_share) ? NULL : "expected a fraction from 0 to 1";
    }
    if (strcmp(key, "churn.nodes") == 0) {
        return parse_kinds(v, &s->churn_nodes) ? NULL : "expected all, relays or leaves";
    }
    if (strcmp(key, "churn.up") == 0) {
        return parse_time(v, &s->churn_up) && s->churn_up > 0 ? NULL
                                                              : "expected a time, such as 2 h";
    }
    if (strcmp(key, "churn.down") == 0) {
        return parse_time(v, &s->churn_down) && s->churn_down > 0
                   ? NULL
                   : "expected a time, such as 15 min";
    }
    if (strcmp(key, "move.share") == 0) {
        return parse_fraction(v, &s->move_share) ? NULL : "expected a fraction from 0 to 1";
    }
    if (strcmp(key, "move.nodes") == 0) {
        return parse_kinds(v, &s->move_nodes) ? NULL : "expected all, relays or leaves";
    }
    if (strcmp(key, "move.speed_min") == 0 || strcmp(key, "move.speed_max") == 0) {
        double *out = strcmp(key, "move.speed_min") == 0 ? &s->move_speed_min : &s->move_speed_max;
        return parse_double(v, out) && *out > 0 && *out <= 1e6
                   ? NULL
                   : "expected a speed above 0, in metres a second";
    }
    if (strcmp(key, "move.pause") == 0) {
        return parse_time(v, &s->move_pause) ? NULL : "expected a time, such as 5 min";
    }
    if (strcmp(key, "move.step") == 0) {
        return parse_time(v, &s->move_step) && s->move_step > 0 ? NULL
                                                                : "expected a time, such as 10 s";
    }
    return "is not a setting";
}

/* --- Reading a file --- */

struct entry {
    int line;
    char *key;
    char *value;
};

static bool fail(struct tsim_scenario_error *err, int line, const char *fmt, ...) {
    err->line = line;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->message, sizeof err->message, fmt, ap);
    va_end(ap);
    return false;
}

static char *trim(char *s) {
    s = (char *)skip_space(s);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s;
}

/* Splits the text into settings, in place. Returns the number found, or -1 after filling err. */
static long split(char *text, struct entry *entries, struct tsim_scenario_error *err) {
    long count = 0;
    int line = 0;
    for (char *next = text; next;) {
        char *s = next;
        line++;
        next = strchr(s, '\n');
        if (next) {
            *next++ = '\0';
        }
        char *hash = strchr(s, '#');
        if (hash) {
            *hash = '\0';
        }
        s = trim(s);
        if (*s == '\0') {
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            fail(err, line, "expected a setting, as key = value");
            return -1;
        }
        *eq = '\0';
        char *key = trim(s);
        char *value = trim(eq + 1);
        if (*key == '\0' || *value == '\0') {
            if (*key) {
                fail(err, line, "%s has no value", key);
            } else {
                fail(err, line, "a value has no setting");
            }
            return -1;
        }
        entries[count++] = (struct entry){line, key, value};
    }
    return count;
}

/* How far from the origin a node can stand. */
static double extent(const struct tsim_scenario *s) {
    switch (s->placement) {
    case TSIM_PLACEMENT_GRID: {
        uint32_t cols = 1;
        while ((uint64_t)cols * cols < s->nodes) {
            cols++;
        }
        return s->spacing_m * (double)(cols - 1);
    }
    case TSIM_PLACEMENT_LINE:
        return s->spacing_m * (double)(s->nodes - 1);
    case TSIM_PLACEMENT_FILE:
        return 0; /* tsim_scenario_read_positions() checks each position */
    case TSIM_PLACEMENT_UNIFORM:
        break;
    }
    return s->width_m > s->height_m ? s->width_m : s->height_m;
}

static bool plugin_key(const char *key, const char *prefix, const char **rest) {
    size_t len = strlen(prefix);
    if (strncmp(key, prefix, len) == 0) {
        *rest = key + len;
        return true;
    }
    return false;
}

bool tsim_scenario_parse(struct tsim_scenario *s, const char *text,
                         struct tsim_scenario_error *err) {
    *err = (struct tsim_scenario_error){0};
    defaults(s);
    char *copy = malloc(strlen(text) + 1);
    /* Never more settings than lines. */
    size_t lines = 1;
    for (const char *c = text; *c; c++) {
        lines += *c == '\n';
    }
    struct entry *entries = malloc(lines * sizeof *entries);
    if (!copy || !entries) {
        free(copy);
        free(entries);
        return fail(err, 0, "out of memory");
    }
    strcpy(copy, text);
    long count = split(copy, entries, err);
    bool ok = count >= 0;

    /* The scenario's own settings first, so the plugins are chosen and the radio is known before
     * any plugin setting is applied, wherever in the file it was written. */
    const char *rest;
    for (long i = 0; ok && i < count; i++) {
        const struct entry *e = &entries[i];
        if (plugin_key(e->key, "routing.", &rest) || plugin_key(e->key, "mac.", &rest)) {
            continue;
        }
        const char *why = set_core(s, e->key, e->value);
        if (why) {
            ok = fail(err, e->line, "%s = %s: %s", e->key, e->value, why);
        }
    }
    if (ok && s->nodes == 0) {
        ok = fail(err, 0, "nodes is not set");
    }
    if (ok && !s->routing) {
        ok = fail(err, 0, "routing is not set");
    }
    if (ok && !s->mac) {
        ok = fail(err, 0, "mac is not set");
    }
    if (ok && s->placement == TSIM_PLACEMENT_FILE && !s->positions_file[0] && !s->links_file[0]) {
        ok = fail(err, 0, "positions is not set: placement = file reads them from that file");
    }
    for (uint32_t i = 0; ok && i < s->send_count; i++) {
        const struct tsim_send *send = &s->sends[i];
        if (send->src >= s->nodes || (send->dst != TSIM_BROADCAST && send->dst >= s->nodes)) {
            ok =
                fail(err, 0, "traffic.send %" PRIu32 " names a node past the %" PRIu32 " there are",
                     i + 1, s->nodes);
        } else if (send->src == send->dst) {
            ok = fail(err, 0, "traffic.send %" PRIu32 " is from a node to itself", i + 1);
        }
    }
    if (ok && s->peers > s->nodes - 1) {
        ok = fail(err, 0, "traffic.peers is %" PRIu32 ", more than the %" PRIu32 " other nodes",
                  s->peers, s->nodes - 1);
    }
    if (ok && s->reply > 0 && s->closed) {
        ok = fail(err, 0,
                  "traffic.reply needs traffic.closed = no: an answer would start no gap, and "
                  "the closed loop has no gap for it to wait on");
    }
    if (ok && !(extent(s) / s->channel.decorrelation_m <= 1e9)) {
        ok = fail(err, 0,
                  "the map is over a billion channel.decorrelation cells across, too many for a "
                  "position to be placed within its cell");
    }
    if (ok && (s->warmup > INT64_MAX - s->duration ||
               s->deadline > INT64_MAX - s->warmup - s->duration)) {
        ok = fail(err, 0, "warmup, duration and deadline together are too long");
    }
    if (ok && s->lead > s->warmup) {
        ok = fail(err, 0, "traffic.lead is longer than the warmup");
    }
    for (uint32_t i = 0; ok && i < s->send_count; i++) {
        if (s->sends[i].at >= s->warmup + s->duration + s->deadline) {
            ok = fail(err, 0, "traffic.send %" PRIu32 " is at or after the end of the run", i + 1);
        } else if (s->sends[i].at < s->warmup) {
            /* Made to be counted, and only messages made after the warmup are. */
            ok = fail(err, 0, "traffic.send %" PRIu32 " is during the warmup", i + 1);
        }
    }
    if (ok) {
        s->routing->defaults(s->routing_config, &s->radio);
        s->mac->defaults(s->mac_config, &s->radio);
    }
    for (long i = 0; ok && i < count; i++) {
        const struct entry *e = &entries[i];
        const char *why;
        if (plugin_key(e->key, "routing.", &rest)) {
            why = s->routing->set(s->routing_config, rest, e->value);
        } else if (plugin_key(e->key, "mac.", &rest)) {
            why = s->mac->set(s->mac_config, rest, e->value);
        } else {
            continue;
        }
        if (why) {
            ok = fail(err, e->line, "%s = %s: %s", e->key, e->value, why);
        }
    }
    const char *why;
    if (ok && s->routing->check && (why = s->routing->check(s->routing_config))) {
        ok = fail(err, 0, "routing %s: %s", s->routing->name, why);
    }
    if (ok && s->mac->check && (why = s->mac->check(s->mac_config))) {
        ok = fail(err, 0, "mac %s: %s", s->mac->name, why);
    }
    const struct tsim_distvec_config *dv =
        ok && s->routing->routing == &tsim_distvec ? (const void *)s->routing_config : NULL;
    struct picked pk =
        ok ? picked_of(s->routing->routing, (void *)s->routing_config) : (struct picked){0};
    if (ok && pk.pick && *pk.pick == TSIM_DISTVEC_PICK_SPACED && s->links_file[0]) {
        ok = fail(err, 0, "routing.relay_pick = spaced needs positions, which links replaces");
    }
    if (ok && s->move_share > 0 && s->move_speed_min > s->move_speed_max) {
        ok = fail(err, 0, "move.speed_min is above move.speed_max");
    }
    if (ok && s->move_share > 0 && s->links_file[0]) {
        ok = fail(err, 0, "move.share needs positions, which links replaces");
    }
    if (ok && s->move_share > 0 && dv && (dv->oracle || dv->links == TSIM_DISTVEC_LINKS_ORACLE)) {
        ok = fail(err, 0,
                  "move.share: the oracle's routes are built once, from where the nodes start");
    }
    /* Elected relays are not known when churn and movement pick their nodes, at the start. */
    if (ok && dv && dv->relay_pick == TSIM_DISTVEC_PICK_ELECT &&
        ((s->churn_share > 0 && s->churn_nodes != TSIM_CHURN_ALL) ||
         (s->move_share > 0 && s->move_nodes != TSIM_CHURN_ALL))) {
        ok = fail(err, 0,
                  "churn.nodes and move.nodes must be all with routing.relay_pick = elect: which "
                  "nodes are relays is not known until they elect themselves");
    }
    if (ok && pk.pick && *pk.pick != TSIM_DISTVEC_PICK_LIST && *pk.count > s->nodes) {
        ok = fail(err, 0, "routing.relay_count is %" PRIu32 ", more than the %" PRIu32 " nodes",
                  *pk.count, s->nodes);
    }
    free(entries);
    free(copy);
    return ok;
}

/* --- Positions --- */

/* Reads one coordinate at *p, moving past it and any separator after it. */
static bool coordinate(const char **p, double *out) {
    char *end;
    *out = strtod(*p, &end);
    if (end == *p) {
        return false;
    }
    *p = skip_space(end);
    if (**p == ',') {
        *p = skip_space(*p + 1);
    }
    return true;
}

bool tsim_scenario_read_positions(const struct tsim_scenario *s, const char *text,
                                  struct tsim_pos *out, struct tsim_scenario_error *err) {
    /* Past a billion cells, a position can no longer be placed within its cell. */
    double limit = 1e9 * s->channel.decorrelation_m;
    uint32_t count = 0;
    int line = 0;
    for (const char *at = text; *at;) {
        line++;
        const char *next = strchr(at, '\n');
        size_t len = next ? (size_t)(next - at) : strlen(at);
        char buf[256];
        if (len >= sizeof buf) {
            return fail(err, line, "the line is too long");
        }
        memcpy(buf, at, len);
        buf[len] = '\0';
        at += len + (next ? 1 : 0);
        char *hash = strchr(buf, '#');
        if (hash) {
            *hash = '\0';
        }
        const char *p = skip_space(buf);
        if (*p == '\0') {
            continue;
        }
        struct tsim_pos pos;
        if (!coordinate(&p, &pos.x) || !coordinate(&p, &pos.y) || *p != '\0') {
            return fail(err, line, "expected a position, as x y in metres");
        }
        if (!(fabs(pos.x) <= limit && fabs(pos.y) <= limit)) {
            return fail(
                err, line,
                "the position is over a billion channel.decorrelation cells from the origin");
        }
        if (count == s->nodes) {
            return fail(err, line, "there are more positions than the %" PRIu32 " nodes", s->nodes);
        }
        out[count++] = pos;
    }
    if (count < s->nodes) {
        return fail(err, 0, "there are %" PRIu32 " positions for %" PRIu32 " nodes", count,
                    s->nodes);
    }
    return true;
}

/* --- Links --- */

static int compare_links(const void *a, const void *b) {
    const struct tsim_link *x = a, *y = b;
    if (x->from != y->from) {
        return x->from < y->from ? -1 : 1;
    }
    return x->to < y->to ? -1 : x->to > y->to;
}

/* Reads one loss at *p, moving past it and the space after it. */
static bool link_loss(const char **p, double *out) {
    char *end;
    *out = strtod(*p, &end);
    if (end == *p || !(fabs(*out) <= 1e3)) {
        return false;
    }
    *p = skip_space(end);
    return true;
}

/* Reads one node number below `nodes` at *p, moving past it and the space after it. */
static bool link_node(const char **p, uint32_t nodes, uint32_t *out) {
    if (!isdigit((unsigned char)**p)) {
        return false;
    }
    char *end;
    errno = 0;
    unsigned long long x = strtoull(*p, &end, 10);
    if (errno == ERANGE || x >= nodes) {
        return false;
    }
    *out = (uint32_t)x;
    *p = skip_space(end);
    return true;
}

static bool links_fail(struct tsim_link *links, size_t *count, struct tsim_scenario_error *err,
                       int line, const char *why) {
    free(links);
    *count = 0;
    return fail(err, line, "%s", why);
}

bool tsim_scenario_read_links(const struct tsim_scenario *s, const char *text,
                              struct tsim_link **out, size_t *count,
                              struct tsim_scenario_error *err) {
    *out = NULL;
    *count = 0;
    size_t cap = 0;
    struct tsim_link *links = NULL;
    int line = 0;
    for (const char *at = text; *at;) {
        line++;
        const char *next = strchr(at, '\n');
        size_t len = next ? (size_t)(next - at) : strlen(at);
        char buf[256];
        if (len >= sizeof buf) {
            return links_fail(links, count, err, line, "the line is too long");
        }
        memcpy(buf, at, len);
        buf[len] = '\0';
        at += len + (next ? 1 : 0);
        char *hash = strchr(buf, '#');
        if (hash) {
            *hash = '\0';
        }
        const char *p = skip_space(buf);
        if (*p == '\0') {
            continue;
        }
        uint32_t a, b;
        double ab, ba;
        if (!link_node(&p, s->nodes, &a) || !link_node(&p, s->nodes, &b)) {
            free(links);
            *count = 0;
            return fail(err, line, "expected two node numbers below %" PRIu32 ", then the loss",
                        s->nodes);
        }
        if (!link_loss(&p, &ab)) {
            return links_fail(links, count, err, line, "expected a loss in dB from -1000 to 1000");
        }
        ba = ab;
        if (*p != '\0' && (!link_loss(&p, &ba) || *p != '\0')) {
            return links_fail(links, count, err, line,
                              "expected at most a second loss, from -1000 to 1000 dB");
        }
        if (a == b) {
            return links_fail(links, count, err, line, "a node is linked to itself");
        }
        if (*count + 2 > cap) {
            cap = cap ? 2 * cap : 64;
            struct tsim_link *grown = realloc(links, cap * sizeof *grown);
            if (!grown) {
                return links_fail(links, count, err, 0, "out of memory");
            }
            links = grown;
        }
        links[(*count)++] = (struct tsim_link){a, b, ab};
        links[(*count)++] = (struct tsim_link){b, a, ba};
    }
    if (*count == 0) {
        return fail(err, 0, "there are no links");
    }
    qsort(links, *count, sizeof *links, compare_links);
    for (size_t i = 1; i < *count; i++) {
        if (links[i].from == links[i - 1].from && links[i].to == links[i - 1].to) {
            uint32_t lo = links[i].from < links[i].to ? links[i].from : links[i].to;
            uint32_t hi = links[i].from < links[i].to ? links[i].to : links[i].from;
            free(links);
            *count = 0;
            return fail(err, 0, "the link between %" PRIu32 " and %" PRIu32 " is given twice", lo,
                        hi);
        }
    }
    *out = links;
    return true;
}

/* --- Running --- */

/* Announces among relays (MSH-60): every ordered pair of relays the oracle links, from the window
 * on. */
struct pair {
    tsim_time heard;                       /* last decoded, or the window's start */
    uint32_t pending[TSIM_PHY_FATE_COUNT]; /* frames since, by fate */
};

struct announces {
    uint32_t n;
    uint32_t relays;
    uint32_t *index;    /* [node]: its relay index, or UINT32_MAX */
    bool *watch;        /* [node]: a relay */
    uint8_t *linked;    /* [relay * relays + relay] */
    struct pair *pairs; /* [relay * relays + relay] */
    uint64_t sent;      /* announce frames relays had sent as the window began */
    struct tsim_announces report;
    double silence_s;
};

static void close_silence(struct announces *a, struct pair *p, tsim_time now) {
    uint64_t sent = 0;
    for (int f = 0; f < TSIM_PHY_FATE_COUNT; f++) {
        sent += p->pending[f];
    }
    if (now - p->heard >= TSIM_SILENCE) {
        a->report.silences++;
        a->silence_s += (double)(now - p->heard) / 1e9;
        if (sent == 0) {
            a->report.unsent++;
        } else if (p->pending[TSIM_PHY_WEAK] == sent) {
            a->report.quiet++;
        } else {
            for (int f = 0; f < TSIM_PHY_FATE_COUNT; f++) {
                a->report.lost[f] += p->pending[f];
            }
        }
    }
    memset(p->pending, 0, sizeof p->pending);
}

static void announce_heard(void *ctx, const struct tsim_net_heard *h) {
    struct announces *a = ctx;
    if (h->purpose != TSIM_PURPOSE_ANNOUNCE || a->index[h->from] == UINT32_MAX) {
        return;
    }
    uint32_t from = a->index[h->from], to = a->index[h->to];
    size_t k = (size_t)from * a->relays + to;
    if (!a->linked[k]) {
        return;
    }
    a->report.heard[h->fate]++;
    struct pair *p = &a->pairs[k];
    if (h->fate == TSIM_PHY_DECODED) {
        close_silence(a, p, h->start);
        p->heard = h->start;
    } else {
        p->pending[h->fate]++;
    }
}

/* Readies the watch over relays linked as the oracle has them: both ends decoding the other at
 * tx_dbm with oracle_margin_db to spare. */
static bool announces_start(struct announces *a, struct tsim_net *net,
                            const struct tsim_distvec_config *dv, tsim_time now) {
    uint32_t n = tsim_net_nodes(net);
    const struct tsim_phy *phy = tsim_net_phy(net);
    a->n = n;
    a->index = malloc(n * sizeof *a->index);
    a->watch = calloc(n, sizeof *a->watch);
    if (!a->index || !a->watch) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        /* Elected relays are known only now, from the routers themselves. */
        const void *r = tsim_net_routing(net, i);
        a->watch[i] = dv->relay_pick == TSIM_DISTVEC_PICK_ELECT ? r && tsim_distvec_infra(r)
                                                                : tsim_distvec_relay(dv, i);
        a->index[i] = a->watch[i] ? a->relays++ : UINT32_MAX;
    }
    size_t rr = (size_t)a->relays * a->relays;
    a->linked = calloc(rr ? rr : 1, 1);
    a->pairs = calloc(rr ? rr : 1, sizeof *a->pairs);
    if (!a->linked || !a->pairs) {
        return false;
    }
    double top = dv->tx_dbm - dv->oracle_margin_db - tsim_phy_floor_dbm(phy, &dv->lora);
    for (uint32_t x = 0; x < n; x++) {
        for (uint32_t y = 0; y < n; y++) {
            if (x != y && a->watch[x] && a->watch[y] && tsim_phy_loss(phy, x, y) <= top &&
                tsim_phy_loss(phy, y, x) <= top) {
                size_t k = (size_t)a->index[x] * a->relays + a->index[y];
                a->linked[k] = 1;
                a->report.links++;
                a->pairs[k].heard = now;
            }
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        a->sent += a->watch[i] ? tsim_net_ledger(net, i)->frames[TSIM_PURPOSE_ANNOUNCE] : 0;
    }
    tsim_net_observe_heard(net, announce_heard, a, a->watch);
    return true;
}

/* Counts the links relays use against the oracle's. */
static void sensed_links(const struct announces *a, struct tsim_net *net, uint32_t out[3]) {
    out[0] = out[1] = out[2] = 0;
    for (uint32_t x = 0; x < a->n; x++) {
        const void *r = tsim_net_routing(net, x); /* NULL while down: it uses no link */
        for (uint32_t y = 0; a->watch[x] && y < a->n; y++) {
            if (x == y || !a->watch[y]) {
                continue;
            }
            bool used = r && tsim_distvec_uses(r, y);
            bool true_link = a->linked[(size_t)a->index[x] * a->relays + a->index[y]];
            out[used && true_link ? 0 : used ? 1 : 2] += used || true_link;
        }
    }
}

static void announces_report(struct announces *a, struct tsim_net *net, tsim_time begun,
                             tsim_time end, struct tsim_announces *out) {
    /* From the senders' own books, so a lone relay's count too - less what is still on the air,
     * whose fates are not yet known, so that it is in neither the frames sent nor the silences. */
    uint64_t sent = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        if (a->watch[i]) {
            const struct tsim_tx *air = tsim_net_on_air(net, i);
            sent += tsim_net_ledger(net, i)->frames[TSIM_PURPOSE_ANNOUNCE] -
                    (air && air->purpose == TSIM_PURPOSE_ANNOUNCE);
        }
    }
    for (size_t k = 0; k < (size_t)a->relays * a->relays; k++) {
        if (a->linked[k]) {
            a->report.silent_end += end - a->pairs[k].heard >= TSIM_SILENCE;
            close_silence(a, &a->pairs[k], end);
        }
    }
    *out = a->report;
    out->present = true;
    double hours = (double)(end - begun) / (double)TSIM_S(3600);
    out->sent_per_h = a->relays && hours > 0 ? (double)(sent - a->sent) / a->relays / hours : 0;
    out->silence_mean_s = a->report.silences ? a->silence_s / (double)a->report.silences : 0;
}

static void announces_free(struct announces *a) {
    free(a->index);
    free(a->watch);
    free(a->linked);
    free(a->pairs);
}

/* What the window's start leaves for its end to be measured against. */
struct window {
    struct tsim_metrics *metrics;
    struct tsim_net *net;
    const struct tsim_distvec_config *dv;     /* NULL for other routing */
    bool *relay;                              /* [node], with dv */
    const struct tsim_distvec_stats *retired; /* books of routers since powered down */
    struct announces *announces;              /* with report.announces */
    bool failed;
    tsim_time begun;
    struct tsim_distvec_stats stats;
    double relay_reach;
};

/* Every distvec node's books, summed. */
static void distvec_sum(struct tsim_net *net, const struct tsim_distvec_stats *retired,
                        struct tsim_distvec_stats *sum) {
    *sum = retired ? *retired : (struct tsim_distvec_stats){0};
    for (uint32_t i = 0; i < tsim_net_nodes(net); i++) {
        struct tsim_distvec_stats s;
        if (!tsim_net_routing(net, i)) {
            continue; /* powered down */
        }
        tsim_distvec_stats(tsim_net_routing(net, i), &s);
        for (int c = 0; c < TSIM_DISTVEC_DOWN_COUNT; c++) {
            sum->down[c] += s.down[c];
            sum->down_strong[c] += s.down_strong[c];
        }
        sum->outages += s.outages;
        sum->unrouted_s += s.unrouted_s;
        sum->urgent_s += s.urgent_s;
        sum->seqno_requests += s.seqno_requests;
        sum->route_requests += s.route_requests;
        sum->gave_up += s.gave_up;
        sum->seq_raised += s.seq_raised;
        sum->route_replies += s.route_replies;
        sum->probes += s.probes;
        sum->probes_answered += s.probes_answered;
        sum->probe_acks += s.probe_acks;
        sum->solicits += s.solicits;
        sum->heres += s.heres;
        sum->reattached += s.reattached;
        sum->unrouted_infeasible += s.unrouted_infeasible;
        sum->unrouted_empty += s.unrouted_empty;
    }
}

/* The share of ordered pairs of relays whose routes, followed node to node as frames go, get from
 * one to the other: a loop, or more hops than nodes, does not. */
static double relay_reach(struct tsim_net *net, const bool *relay) {
    uint32_t n = tsim_net_nodes(net);
    uint64_t pairs = 0, reach = 0;
    for (uint32_t src = 0; src < n; src++) {
        for (uint32_t dst = 0; dst < n; dst++) {
            if (src == dst || !relay[src] || !relay[dst]) {
                continue;
            }
            pairs++;
            uint32_t at = src, next, hops = 0;
            while (at != dst && hops++ < n && tsim_net_routing(net, at) &&
                   tsim_distvec_next(tsim_net_routing(net, at), dst, &next)) {
                at = next;
            }
            reach += at == dst;
        }
    }
    return pairs ? (double)reach / (double)pairs : 0;
}

/* With relay_pick elect, which nodes are relays now: as they elected themselves. */
static void elected(struct window *w) {
    if (!w->dv || w->dv->relay_pick != TSIM_DISTVEC_PICK_ELECT) {
        return;
    }
    for (uint32_t i = 0; i < tsim_net_nodes(w->net); i++) {
        const void *r = tsim_net_routing(w->net, i);
        w->relay[i] = r && tsim_distvec_infra(r);
    }
}

static void begin_window(struct tsim_sched *sched, void *ctx) {
    (void)sched;
    struct window *w = ctx;
    tsim_metrics_begin(w->metrics);
    w->begun = tsim_sched_now(sched);
    elected(w);
    if (w->dv) {
        distvec_sum(w->net, w->retired, &w->stats);
        w->relay_reach = relay_reach(w->net, w->relay);
    }
    if (w->announces && !announces_start(w->announces, w->net, w->dv, w->begun)) {
        w->failed = true;
    } else if (w->announces) {
        sensed_links(w->announces, w->net, w->announces->report.sensed[0]);
    }
}

/* Candidate 3's health over the window, from its books now against the window's start. */
static void health(struct window *w, tsim_time end, struct tsim_route_health *h) {
    elected(w);
    struct tsim_distvec_stats now;
    distvec_sum(w->net, w->retired, &now);
    double hours = (double)(end - w->begun) / (double)TSIM_S(3600);
    double seconds = hours * 3600;
    uint32_t relays = 0;
    for (uint32_t i = 0; i < tsim_net_nodes(w->net); i++) {
        relays += w->relay[i];
    }
    *h = (struct tsim_route_health){.present = true};
    _Static_assert(TSIM_HEALTH_CAUSES == TSIM_DISTVEC_DOWN_COUNT, "link-down causes");
    for (int c = 0; c < TSIM_DISTVEC_DOWN_COUNT; c++) {
        h->down_per_h[c] = hours > 0 ? (double)(now.down[c] - w->stats.down[c]) / hours : 0;
        h->strong_per_h[c] =
            hours > 0 ? (double)(now.down_strong[c] - w->stats.down_strong[c]) / hours : 0;
    }
    uint64_t outages = now.outages - w->stats.outages;
    double unrouted = now.unrouted_s - w->stats.unrouted_s;
    h->outages_per_h = hours > 0 ? (double)outages / hours : 0;
    h->outage_mean_s = outages ? unrouted / (double)outages : 0;
    if (relays && seconds > 0) {
        h->unrouted_mean = unrouted / seconds / relays;
        h->urgent_mean = (now.urgent_s - w->stats.urgent_s) / seconds / relays;
    }
    if (hours > 0) {
        h->seqno_requests_per_h = (double)(now.seqno_requests - w->stats.seqno_requests) / hours;
        h->route_requests_per_h = (double)(now.route_requests - w->stats.route_requests) / hours;
        h->gave_up_per_h = (double)(now.gave_up - w->stats.gave_up) / hours;
        h->seq_raised_per_h = (double)(now.seq_raised - w->stats.seq_raised) / hours;
        h->route_replies_per_h = (double)(now.route_replies - w->stats.route_replies) / hours;
        h->probes_per_h = (double)(now.probes - w->stats.probes) / hours;
        h->probes_answered_per_h = (double)(now.probes_answered - w->stats.probes_answered) / hours;
        h->probe_acks_per_h = (double)(now.probe_acks - w->stats.probe_acks) / hours;
        h->solicits_per_h = (double)(now.solicits - w->stats.solicits) / hours;
        h->heres_per_h = (double)(now.heres - w->stats.heres) / hours;
        h->reattached_per_h = (double)(now.reattached - w->stats.reattached) / hours;
    }
    if (relays) {
        h->unrouted_infeasible = (double)now.unrouted_infeasible / relays;
        h->unrouted_empty = (double)now.unrouted_empty / relays;
    }
    h->relay_reach_begin = w->relay_reach;
    h->relay_reach_end = relay_reach(w->net, w->relay);
    _Static_assert(sizeof h->listen_sf / sizeof *h->listen_sf == TSIM_SF_COUNT, "SFs");
    for (uint32_t i = 0; i < tsim_net_nodes(w->net); i++) {
        const void *r = tsim_net_routing(w->net, i);
        if (r) {
            h->listen_sf[tsim_distvec_listen_sf(r) - TSIM_SF_MIN]++;
        }
    }
}

/* --- Churn --- */

struct churn;

struct churner {
    struct churn *churn;
    uint32_t node;
    struct tsim_rng rng;
    bool down;
};

struct churn {
    struct tsim_net *net;
    struct tsim_metrics *metrics;
    tsim_time up;
    tsim_time down;
    bool dv;
    struct tsim_distvec_stats retired; /* what routers powered down had booked */
    struct churner *churners;
    uint32_t count;
    bool failed;
};

/* An exponential draw of mean `mean`, at least 1 ms. */
static tsim_time exponential(struct tsim_rng *rng, tsim_time mean) {
    double t = -log1p(-tsim_rng_unit(rng)) * (double)mean;
    return t < (double)TSIM_MS(1) ? TSIM_MS(1) : t > 1e18 ? (tsim_time)1e18 : (tsim_time)t;
}

static void churn_flip(struct tsim_sched *sched, void *ctx) {
    struct churner *ch = ctx;
    struct churn *c = ch->churn;
    bool on = ch->down;
    if (!on && c->dv) {
        /* Its books go with it, but what it did stays done. */
        struct tsim_distvec_stats st;
        tsim_distvec_stats(tsim_net_routing(c->net, ch->node), &st);
        for (int k = 0; k < TSIM_DISTVEC_DOWN_COUNT; k++) {
            c->retired.down[k] += st.down[k];
            c->retired.down_strong[k] += st.down_strong[k];
        }
        c->retired.outages += st.outages;
        c->retired.unrouted_s += st.unrouted_s;
        c->retired.urgent_s += st.urgent_s;
        c->retired.seqno_requests += st.seqno_requests;
        c->retired.route_requests += st.route_requests;
        c->retired.gave_up += st.gave_up;
        c->retired.seq_raised += st.seq_raised;
        c->retired.route_replies += st.route_replies;
        c->retired.probes += st.probes;
        c->retired.probes_answered += st.probes_answered;
        c->retired.probe_acks += st.probe_acks;
        c->retired.solicits += st.solicits;
        c->retired.heres += st.heres;
        c->retired.reattached += st.reattached;
    }
    if (!tsim_net_power(c->net, ch->node, on) || !tsim_metrics_power(c->metrics, ch->node, on)) {
        c->failed = true;
        return;
    }
    ch->down = !on;
    tsim_time next = exponential(&ch->rng, on ? c->up : c->down);
    if (tsim_sched_after(sched, next, churn_flip, ch).slot == 0) {
        c->failed = true;
    }
}

/* Whether `node` is of the kind `kinds` names (enum tsim_churn_nodes). */
static bool of_kind(const struct tsim_scenario *s, const void *rc, uint8_t kinds, uint32_t node) {
    if (kinds == TSIM_CHURN_ALL) {
        return true;
    }
    bool relay = true;
    if (s->routing->routing == &tsim_distvec) {
        relay = tsim_distvec_relay(rc, node);
    } else if (s->routing->routing == &tsim_meshcore) {
        relay = tsim_meshcore_relay(rc, node) == 1;
    } else if (s->routing->routing == &tsim_meshtastic) {
        const struct tsim_meshtastic_config *mt = rc;
        relay = !mt->relay_pick || mt->relay_set[node];
    }
    return relay == (kinds == TSIM_CHURN_RELAYS);
}

/* round(share x eligible) of the nodes of the kind `kinds` names, drawn from `stream`: returned
 * first in a list of the node count, NULL when memory runs out, with how many in *k. */
static uint32_t *pick(const struct tsim_scenario *s, const void *rc, uint8_t kinds, double share,
                      uint64_t stream, uint32_t *k) {
    uint32_t n = s->nodes, eligible = 0;
    uint32_t *pool = malloc(n * sizeof *pool);
    if (!pool) {
        return NULL;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (of_kind(s, rc, kinds, i)) {
            pool[eligible++] = i;
        }
    }
    *k = (uint32_t)(share * eligible + 0.5);
    struct tsim_rng rng;
    tsim_rng_init(&rng, s->seed, stream);
    for (uint32_t i = 0; i < *k; i++) {
        uint32_t j = i + (uint32_t)tsim_rng_below(&rng, eligible - i);
        uint32_t t = pool[i];
        pool[i] = pool[j];
        pool[j] = t;
    }
    return pool;
}

/* Picks round(share x eligible) nodes to churn, and schedules each one's first time down. */
static bool churn_start(struct churn *c, const struct tsim_scenario *s, const void *rc,
                        struct tsim_sched *sched) {
    uint32_t k;
    uint32_t *pool = pick(s, rc, s->churn_nodes, s->churn_share, UINT64_C(0xC4) << 56, &k);
    if (!pool) {
        return false;
    }
    c->churners = calloc(k ? k : 1, sizeof *c->churners);
    if (!c->churners) {
        free(pool);
        return false;
    }
    c->count = k;
    bool ok = true;
    for (uint32_t i = 0; i < k && ok; i++) {
        struct churner *ch = &c->churners[i];
        *ch = (struct churner){.churn = c, .node = pool[i]};
        tsim_rng_init(&ch->rng, s->seed, UINT64_C(0xC5) << 56 | pool[i]);
        ok = tsim_sched_after(sched, exponential(&ch->rng, c->up), churn_flip, ch).slot != 0;
    }
    free(pool);
    return ok;
}

/* --- Movement --- */

struct moving {
    struct tsim_phy *phy;
    struct tsim_channel_params channel;
    struct tsim_move_params params;
    tsim_time step;
    uint32_t nodes;
    struct tsim_channel_spot *spot; /* [node] */
    uint32_t count;
    uint32_t *who;             /* [mover]: its node */
    struct tsim_mover *movers; /* [mover] */
    bool *moved;               /* [mover]: this step */
    double *own;               /* [mover * nodes + node]: the link's own shadowing */
    bool failed;
};

static void move_step(struct tsim_sched *sched, void *ctx) {
    struct moving *mv = ctx;
    for (uint32_t i = 0; i < mv->count; i++) {
        mv->moved[i] = tsim_mover_step(&mv->movers[i], &mv->params, mv->step);
        if (mv->moved[i]) {
            mv->spot[mv->who[i]] = tsim_channel_spot(&mv->channel, mv->movers[i].at);
        }
    }
    /* Every spot is where it now is before any loss is set, so a link between two movers comes
     * out the same whichever is set last. */
    for (uint32_t i = 0; i < mv->count; i++) {
        if (!mv->moved[i]) {
            continue;
        }
        uint32_t a = mv->who[i];
        const double *own = &mv->own[(size_t)i * mv->nodes];
        for (uint32_t b = 0; b < mv->nodes; b++) {
            if (b != a) {
                tsim_phy_set_loss(
                    mv->phy, a, b,
                    tsim_channel_spot_loss(&mv->channel, &mv->spot[a], &mv->spot[b], own[b]));
            }
        }
    }
    if (tsim_sched_after(sched, mv->step, move_step, mv).slot == 0) {
        mv->failed = true;
    }
}

/* Picks the movers, starts each where it stands, and schedules the first step. */
static bool move_start(struct moving *mv, const struct tsim_scenario *s, const void *rc,
                       struct tsim_sched *sched, struct tsim_phy *phy, const struct tsim_pos *pos) {
    uint32_t n = s->nodes;
    *mv = (struct moving){
        .phy = phy,
        .channel = s->channel,
        .params = {.speed_min = s->move_speed_min,
                   .speed_max = s->move_speed_max,
                   .pause = s->move_pause},
        .step = s->move_step,
        .nodes = n,
    };
    mv->channel.seed = s->seed;
    if (s->placement == TSIM_PLACEMENT_UNIFORM) {
        mv->params.x_max = s->width_m;
        mv->params.y_max = s->height_m;
    } else {
        mv->params.x_min = mv->params.x_max = pos[0].x;
        mv->params.y_min = mv->params.y_max = pos[0].y;
        for (uint32_t i = 1; i < n; i++) {
            mv->params.x_min = fmin(mv->params.x_min, pos[i].x);
            mv->params.x_max = fmax(mv->params.x_max, pos[i].x);
            mv->params.y_min = fmin(mv->params.y_min, pos[i].y);
            mv->params.y_max = fmax(mv->params.y_max, pos[i].y);
        }
    }
    mv->who = pick(s, rc, s->move_nodes, s->move_share, UINT64_C(0xC6) << 56, &mv->count);
    uint32_t k = mv->count ? mv->count : 1;
    mv->spot = malloc(n * sizeof *mv->spot);
    mv->movers = malloc(k * sizeof *mv->movers);
    mv->moved = malloc(k * sizeof *mv->moved);
    mv->own = malloc((size_t)k * n * sizeof *mv->own);
    if (!mv->who || !mv->spot || !mv->movers || !mv->moved || !mv->own) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        mv->spot[i] = tsim_channel_spot(&mv->channel, pos[i]);
    }
    for (uint32_t i = 0; i < mv->count; i++) {
        uint32_t a = mv->who[i];
        tsim_mover_init(&mv->movers[i], &mv->params, pos[a], s->seed, a);
        for (uint32_t b = 0; b < n; b++) {
            mv->own[(size_t)i * n + b] = b == a ? 0 : tsim_channel_own(&mv->channel, a, b);
        }
    }
    tsim_phy_keep_losses(phy, true);
    return tsim_sched_after(sched, mv->step, move_step, mv).slot != 0;
}

static void move_free(struct moving *mv) {
    free(mv->who);
    free(mv->spot);
    free(mv->movers);
    free(mv->moved);
    free(mv->own);
}

/* Sets every link's loss as the scenario says: from its links file, or the channel model. */
static bool lay_links(struct tsim_phy *phy, const struct tsim_scenario *s,
                      const struct tsim_pos *pos) {
    if (s->links_file[0]) {
        if (!s->links) {
            return false;
        }
        for (size_t i = 0; i < s->link_count; i++) {
            tsim_phy_set_loss_from(phy, s->links[i].from, s->links[i].to, s->links[i].loss_db);
        }
    } else {
        struct tsim_channel_params channel = s->channel;
        channel.seed = s->seed;
        tsim_phy_set_losses(phy, &channel, pos);
    }
    return true;
}

bool tsim_scenario_run(const struct tsim_scenario *s, struct tsim_report *report) {
    struct tsim_pos *pos = malloc(s->nodes * sizeof *pos);
    struct tsim_sched *sched = tsim_sched_create();
    struct tsim_net *net = NULL;
    struct tsim_metrics *metrics = NULL;
    struct tsim_traffic *traffic = NULL;
    struct tsim_distvec_oracle oracle = {0};
    uint32_t *parents = NULL;
    uint8_t *relay_set = NULL;
    struct window window = {0};
    struct churn churn = {0};
    struct moving moving = {0};
    struct announces announces = {0};
    bool ok = false;
    /* The oracle's routes are the driver's to hand down: the plugin is given where they will be,
     * and they are built once the links are laid. So is the table leaves' parents are kept in. */
    _Alignas(max_align_t) unsigned char routing_config[TSIM_PLUGIN_CONFIG_MAX];
    memcpy(routing_config, s->routing_config, sizeof routing_config);
    struct tsim_distvec_config *dv = NULL;
    if (s->routing->routing == &tsim_distvec) {
        dv = (struct tsim_distvec_config *)routing_config;
        dv->oracle_routes = dv->oracle || dv->links == TSIM_DISTVEC_LINKS_ORACLE ? &oracle : NULL;
        if (dv->leaves == TSIM_DISTVEC_LEAVES_PARENT_ORACLE) {
            parents = malloc(s->nodes * sizeof *parents);
            dv->parents = parents;
            if (!parents) {
                goto done;
            }
        }
    }
    if (!pos || !sched) {
        goto done;
    }
    switch (s->placement) {
    case TSIM_PLACEMENT_UNIFORM:
        tsim_place_uniform(pos, s->nodes, s->width_m, s->height_m, s->seed);
        break;
    case TSIM_PLACEMENT_GRID:
        tsim_place_grid(pos, s->nodes, s->spacing_m);
        break;
    case TSIM_PLACEMENT_LINE:
        tsim_place_line(pos, s->nodes, s->spacing_m);
        break;
    case TSIM_PLACEMENT_FILE:
        if (s->links_file[0]) {
            memset(pos, 0, s->nodes * sizeof *pos); /* the links replace them */
        } else if (!s->positions) {
            goto done;
        } else {
            memcpy(pos, s->positions, s->nodes * sizeof *pos);
        }
        break;
    }

    struct tsim_net_params np = s->net;
    np.channel = s->radio.channel;
    np.listen = s->radio.lora;
    np.seed = s->seed;
    /* Picked relays are known before any router is made, so they are picked over a medium of
     * their own with the same losses. Meshcore's repeaters and meshtastic's routers are picked as
     * distvec's infrastructure is, over links at the radio's power and sites.sf and sites.bw. */
    struct picked pk = picked_of(s->routing->routing, routing_config);
    if (pk.pick && *pk.pick != TSIM_DISTVEC_PICK_LIST && *pk.pick != TSIM_DISTVEC_PICK_ELECT) {
        struct tsim_distvec_config by =
            dv ? *dv : tsim_distvec_default(np.channel, &s->radio.lora, s->radio.tx_dbm);
        by.relay_pick = *pk.pick;
        by.relay_count = *pk.count;
        by.lora.sf = s->sites_sf ? s->sites_sf : by.lora.sf;
        by.lora.bw_hz = s->sites_bw ? s->sites_bw : by.lora.bw_hz;
        struct tsim_phy *medium = tsim_phy_create(sched, &np.phy, s->nodes, np.channel, &np.listen,
                                                  (struct tsim_phy_hooks){0});
        relay_set = malloc(s->nodes);
        bool picked = medium && relay_set && lay_links(medium, s, pos) &&
                      tsim_distvec_pick_relays(medium, pos, &by, relay_set);
        tsim_phy_destroy(medium);
        if (!picked) {
            goto done;
        }
        *pk.set = relay_set;
    }
    net = tsim_net_create(sched, &np, s->nodes, s->routing->routing, routing_config, s->mac->mac,
                          s->mac_config);
    if (!net) {
        goto done;
    }
    if (!lay_links(tsim_net_phy(net), s, pos)) {
        goto done;
    }
    if (dv && dv->oracle_routes && !tsim_distvec_oracle_build(&oracle, tsim_net_phy(net), dv)) {
        goto done;
    }

    metrics = tsim_metrics_create(net, s->deadline);
    if (metrics && !tsim_metrics_links(metrics, &s->radio.lora, s->radio.tx_dbm)) {
        goto done;
    }
    window.metrics = metrics;
    window.net = net;
    if (dv && !dv->oracle) {
        window.relay = malloc(s->nodes * sizeof *window.relay);
        if (!window.relay) {
            goto done;
        }
        for (uint32_t i = 0; i < s->nodes; i++) {
            window.relay[i] = tsim_distvec_relay(dv, i);
        }
        window.dv = dv;
        window.announces = s->report_announces ? &announces : NULL;
    }
    /* Scheduled before the traffic, so it runs first of what happens as the warmup ends: a
     * message sent at that instant is all in the window. */
    if (metrics && tsim_sched_at(sched, s->warmup, begin_window, &window).slot == 0) {
        goto done;
    }
    struct tsim_traffic_params tp = {
        .interval = s->interval,
        .len_min = s->len_min,
        .len_max = s->len_max,
        .broadcast = s->broadcast,
        .start = s->warmup - s->lead,
        .stop = s->warmup + s->duration,
        .seed = s->seed,
        .closed = s->closed,
        .peers = s->peers,
        .reply = s->reply,
        .reply_delay = s->reply_delay,
        .sends = s->sends,
        .send_count = s->send_count,
    };
    traffic = tsim_traffic_create(net, &tp);
    if (!metrics || !traffic) {
        goto done;
    }
    if (s->churn_share > 0) {
        churn = (struct churn){.net = net,
                               .metrics = metrics,
                               .up = s->churn_up,
                               .down = s->churn_down,
                               .dv = dv != NULL};
        window.retired = &churn.retired;
        if (!tsim_metrics_churn(metrics) || !churn_start(&churn, s, routing_config, sched)) {
            goto done;
        }
    }
    if (s->move_share > 0 &&
        !move_start(&moving, s, routing_config, sched, tsim_net_phy(net), pos)) {
        goto done;
    }
    tsim_net_start(net);
    tsim_sched_run_until(sched, s->warmup + s->duration + s->deadline);
    tsim_metrics_report(metrics, report);
    if (window.dv) {
        health(&window, tsim_sched_now(sched), &report->health);
    }
    if (window.announces && !window.failed) {
        sensed_links(&announces, net, announces.report.sensed[1]);
        announces_report(&announces, net, window.begun, tsim_sched_now(sched), &report->announces);
    }
    ok = !churn.failed && !moving.failed && !window.failed &&
         tsim_phy_links(tsim_net_phy(net), &s->radio.lora, s->radio.tx_dbm, &report->links);
    if (ok && dv && (dv->relay_pick != TSIM_DISTVEC_PICK_LIST || strcmp(dv->relays, "all") != 0)) {
        uint8_t *now = NULL;
        if (dv->relay_pick == TSIM_DISTVEC_PICK_ELECT) {
            now = malloc(s->nodes);
            for (uint32_t i = 0; now && i < s->nodes; i++) {
                const void *r = tsim_net_routing(net, i);
                now[i] = r && tsim_distvec_infra(r);
            }
        }
        ok = (now || dv->relay_pick != TSIM_DISTVEC_PICK_ELECT) &&
             tsim_distvec_tier(tsim_net_phy(net), dv, now, &report->relays);
        free(now);
    }

done:
    tsim_traffic_destroy(traffic);
    tsim_metrics_destroy(metrics);
    tsim_net_destroy(net);
    tsim_distvec_oracle_free(&oracle);
    free(parents);
    free(relay_set);
    free(churn.churners);
    move_free(&moving);
    announces_free(&announces); /* the network that called into it is gone */
    free(window.relay);
    tsim_sched_destroy(sched);
    free(pos);
    return ok;
}
