#include "tsim/scenario.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tsim/baseline.h"
#include "tsim/meshtastic.h"
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
    return w->cw_min <= w->cw_max ? NULL : "cw_min is over cw_max";
}

static void meshtastic_defaults(void *config, const struct tsim_radio *radio) {
    *(struct tsim_meshtastic_config *)config =
        tsim_meshtastic_default(radio->channel, &radio->lora, radio->tx_dbm);
}

static const char *meshtastic_set(void *config, const char *key, const char *value) {
    struct tsim_meshtastic_config *c = config;
    uint64_t v;
    const char *why = window_set(&c->window, key, value);
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
    if (strcmp(key, "retries") == 0) {
        if (!parse_u64(value, UINT8_MAX, &v)) {
            return "expected a count from 0 to 255";
        }
        c->retries = (uint8_t)v;
        return NULL;
    }
    if (strcmp(key, "processing") == 0) {
        return parse_time(value, &c->processing) ? NULL : "expected a time, such as 4.5 s";
    }
    return "is not a setting of meshtastic";
}

static const char *meshtastic_check(const void *config) {
    return window_check(&((const struct tsim_meshtastic_config *)config)->window);
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
    return "is not a setting of meshtastic";
}

static const char *meshtastic_mac_check(const void *config) {
    const struct tsim_meshtastic_mac_config *c = config;
    if (!(c->snr_min_db < c->snr_max_db)) {
        return "snr_min is not below snr_max";
    }
    return window_check(&c->window);
}

_Static_assert(sizeof(struct tsim_flood_config) <= TSIM_PLUGIN_CONFIG_MAX, "flood config");
_Static_assert(sizeof(struct tsim_aloha_config) <= TSIM_PLUGIN_CONFIG_MAX, "aloha config");
_Static_assert(sizeof(struct tsim_meshtastic_config) <= TSIM_PLUGIN_CONFIG_MAX,
               "meshtastic config");
_Static_assert(sizeof(struct tsim_meshtastic_mac_config) <= TSIM_PLUGIN_CONFIG_MAX,
               "meshtastic mac config");

static const struct tsim_plugin plugins[] = {
    {"flood", &tsim_flood, NULL, sizeof(struct tsim_flood_config), flood_defaults, flood_set, NULL},
    {"aloha", NULL, &tsim_aloha, sizeof(struct tsim_aloha_config), aloha_defaults, aloha_set, NULL},
    {"meshtastic", &tsim_meshtastic, NULL, sizeof(struct tsim_meshtastic_config),
     meshtastic_defaults, meshtastic_set, meshtastic_check},
    {"meshtastic", NULL, &tsim_meshtastic_mac, sizeof(struct tsim_meshtastic_mac_config),
     meshtastic_mac_defaults, meshtastic_mac_set, meshtastic_mac_check},
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
        .warmup = 0,
        .duration = TSIM_S(3600),
        .deadline = TSIM_S(60),
    };
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
        } else {
            return "expected uniform, grid or line";
        }
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

    if (strcmp(key, "traffic.interval") == 0) {
        return parse_time(v, &s->interval) && s->interval > 0 ? NULL : "expected a time above 0";
    }
    if (strcmp(key, "traffic.len") == 0) {
        return parse_range(v, TSIM_FRAME_MAX, &s->len_min, &s->len_max)
                   ? NULL
                   : "expected bytes up to 255, as 32 or 16..64";
    }
    if (strcmp(key, "traffic.broadcast") == 0) {
        return parse_fraction(v, &s->broadcast) ? NULL : "expected a fraction from 0 to 1";
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
    if (ok && !(extent(s) / s->channel.decorrelation_m <= 1e9)) {
        ok = fail(err, 0,
                  "the map is over a billion channel.decorrelation cells across, too many for a "
                  "position to be placed within its cell");
    }
    if (ok && (s->warmup > INT64_MAX - s->duration ||
               s->deadline > INT64_MAX - s->warmup - s->duration)) {
        ok = fail(err, 0, "warmup, duration and deadline together are too long");
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
    free(entries);
    free(copy);
    return ok;
}

/* --- Running --- */

bool tsim_scenario_run(const struct tsim_scenario *s, struct tsim_report *report) {
    struct tsim_pos *pos = malloc(s->nodes * sizeof *pos);
    struct tsim_sched *sched = tsim_sched_create();
    struct tsim_net *net = NULL;
    struct tsim_metrics *metrics = NULL;
    struct tsim_traffic *traffic = NULL;
    bool ok = false;
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
    }

    struct tsim_net_params np = s->net;
    np.channel = s->radio.channel;
    np.listen = s->radio.lora;
    np.seed = s->seed;
    net = tsim_net_create(sched, &np, s->nodes, s->routing->routing, s->routing_config, s->mac->mac,
                          s->mac_config);
    if (!net) {
        goto done;
    }
    struct tsim_channel_params channel = s->channel;
    channel.seed = s->seed;
    tsim_phy_set_losses(tsim_net_phy(net), &channel, pos);

    metrics = tsim_metrics_create(net, s->deadline);
    struct tsim_traffic_params tp = {
        .interval = s->interval,
        .len_min = s->len_min,
        .len_max = s->len_max,
        .broadcast = s->broadcast,
        .start = s->warmup,
        .stop = s->warmup + s->duration,
        .seed = s->seed,
    };
    traffic = tsim_traffic_create(net, &tp);
    if (!metrics || !traffic) {
        goto done;
    }
    tsim_net_start(net);
    tsim_sched_run_until(sched, s->warmup + s->duration + s->deadline);
    tsim_metrics_report(metrics, report);
    ok = true;

done:
    tsim_traffic_destroy(traffic);
    tsim_metrics_destroy(metrics);
    tsim_net_destroy(net);
    tsim_sched_destroy(sched);
    free(pos);
    return ok;
}
