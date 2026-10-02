#include "tsim/scenario.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tsim/baseline.h"
#include "tsim/meshcore.h"
#include "tsim/meshtastic.h"

#include "check.h"

static const struct tsim_flood_config *flood_of(const struct tsim_scenario *s) {
    return (const struct tsim_flood_config *)s->routing_config;
}

static const struct tsim_aloha_config *aloha_of(const struct tsim_scenario *s) {
    return (const struct tsim_aloha_config *)s->mac_config;
}

static bool parse(struct tsim_scenario *s, const char *text) {
    struct tsim_scenario_error err;
    bool ok = tsim_scenario_parse(s, text, &err);
    if (!ok) {
        fprintf(stderr, "  line %d: %s\n", err.line, err.message);
    }
    return ok;
}

/* The example in tsim/scenario.h, as written there. */
static void the_documented_example_parses(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, "# 200 nodes over 5 km, naive flooding\n"
                    "nodes     = 200\n"
                    "placement = uniform\n"
                    "area      = 5000 x 5000      # metres\n"
                    "routing   = flood\n"
                    "routing.hops = 3\n"
                    "mac       = aloha\n"
                    "mac.max_delay = 2 s\n"
                    "traffic.interval = 15 min\n"
                    "duration  = 1 h\n"));
    CHECK(s.nodes == 200 && s.placement == TSIM_PLACEMENT_UNIFORM);
    CHECK(s.width_m == 5000 && s.height_m == 5000);
    CHECK(strcmp(tsim_scenario_routing_name(&s), "flood") == 0);
    CHECK(strcmp(tsim_scenario_mac_name(&s), "aloha") == 0);
    CHECK(flood_of(&s)->hops == 3);
    CHECK_EQ_I64(aloha_of(&s)->max_delay, TSIM_S(2));
    CHECK_EQ_I64(s.interval, TSIM_S(15 * 60));
    CHECK_EQ_I64(s.duration, TSIM_S(3600));
}

static void every_value_kind_reads(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, "nodes = 12\nrouting = flood\nmac = aloha\n"
                    "seed = 18446744073709551615\n"
                    "placement = grid\nspacing = 250.5\n"
                    "radio.channel = 3\nradio.sf = 12\nradio.bw = 62500\nradio.cr = 4\n"
                    "radio.preamble = 16\nradio.tx_dbm = -3.5\n"
                    "channel.pl0 = 120\nchannel.d0 = 100\nchannel.exponent = 3.1\n"
                    "channel.sigma = 0\nchannel.share = 0.25\nchannel.decorrelation = 40\n"
                    "phy.noise_figure = 4\nphy.capture = 3\nphy.lock_symbols = 7\n"
                    "phy.retune = 250 us\n"
                    "queue = 0\n"
                    "traffic.len = 16..64\ntraffic.broadcast = 0\n"
                    "warmup = 1.5 min\nduration = 90 s\ndeadline = 500 ms\n"));
    CHECK_EQ_U64(s.seed, UINT64_MAX);
    CHECK(s.placement == TSIM_PLACEMENT_GRID && s.spacing_m == 250.5);
    CHECK(s.radio.channel == 3 && s.radio.lora.sf == 12 && s.radio.lora.bw_hz == 62500);
    CHECK(s.radio.lora.cr == 4 && s.radio.lora.preamble == 16 && s.radio.tx_dbm == -3.5);
    CHECK(s.channel.pl0_db == 120 && s.channel.d0_m == 100 && s.channel.exponent == 3.1);
    CHECK(s.channel.sigma_db == 0 && s.channel.node_share == 0.25);
    CHECK(s.channel.decorrelation_m == 40);
    CHECK(s.net.phy.noise_figure_db == 4 && s.net.phy.capture_db == 3);
    CHECK(s.net.phy.lock_symbols == 7);
    CHECK_EQ_I64(s.net.phy.retune, TSIM_US(250));
    CHECK(s.net.queue_limit == 0);
    CHECK(s.len_min == 16 && s.len_max == 64 && s.broadcast == 0);
    CHECK_EQ_I64(s.warmup, TSIM_S(90));
    CHECK_EQ_I64(s.duration, TSIM_S(90));
    CHECK_EQ_I64(s.deadline, TSIM_MS(500));
}

/* A plugin's settings may come before the line that chooses it, and its radio follows radio.*
 * wherever that is written. */
static void plugin_settings_may_come_first(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, "routing.hops = 7\nmac.max_delay = 3 s\nnodes = 2\nrouting = flood\n"
                    "mac = aloha\nradio.sf = 10\nradio.tx_dbm = 22\n"));
    CHECK(flood_of(&s)->hops == 7);
    CHECK(flood_of(&s)->lora.sf == 10 && flood_of(&s)->tx_dbm == 22);
    CHECK_EQ_I64(aloha_of(&s)->max_delay, TSIM_S(3));
}

/* What the command line's -s relies on. */
static void a_later_setting_wins(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, "nodes = 5\nrouting = flood\nmac = aloha\nrouting.hops = 2\n"
                    "nodes = 9\nrouting.hops = 4\nseed = 3\n"));
    CHECK(s.nodes == 9 && flood_of(&s)->hops == 4 && s.seed == 3);
}

static void unset_settings_take_their_defaults(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, "nodes = 2\nrouting = flood\nmac = aloha\n"));
    CHECK_EQ_U64(s.seed, 1);
    CHECK(s.placement == TSIM_PLACEMENT_UNIFORM && s.width_m == 5000 && s.height_m == 5000);
    CHECK(s.radio.lora.sf == 7 && s.radio.lora.bw_hz == 125000 && s.radio.tx_dbm == 14);
    CHECK(s.net.queue_limit == 16);
    CHECK(s.len_min == 32 && s.len_max == 32 && s.broadcast == 1.0);
    CHECK_EQ_I64(s.warmup, 0);
    CHECK_EQ_I64(s.duration, TSIM_S(3600));
    CHECK_EQ_I64(s.deadline, TSIM_S(60));
    CHECK(flood_of(&s)->hops == 3);
    CHECK_EQ_I64(aloha_of(&s)->max_delay, TSIM_S(1));
}

/* Each problem is found, and placed on its line - 0 for the scenario as a whole. */
static void problems_say_where_they_are(void) {
    static const struct {
        const char *text;
        int line;
        const char *says;
    } cases[] = {
        {"nodes = 2\nrouting = flood\nmac = aloha\ncolour = blue\n", 4, "is not a setting"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nduration = 5\n", 4, "expected a time"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nduration = 5 fortnights\n", 4,
         "expected a time"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nduration = 0 s\n", 4, "above 0"},
        {"nodes = 2\nrouting = flood\nmac = aloha\ndeadline = -1 s\n", 4, "expected a time"},
        {"nodes = -1\nrouting = flood\nmac = aloha\n", 1, "number of nodes"},
        {"nodes = 0\nrouting = flood\nmac = aloha\n", 1, "number of nodes"},
        {"nodes = 2x\nrouting = flood\nmac = aloha\n", 1, "number of nodes"},
        {"nodes = 99999999999999999999999\nrouting = flood\nmac = aloha\n", 1, "number of nodes"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nseed = 18446744073709551616\n", 4,
         "whole number"},
        {"nodes = 2\nplacement = circle\nrouting = flood\nmac = aloha\n", 2, "uniform, grid"},
        {"nodes = 2\nrouting = ospf\nmac = aloha\n", 2, "not a routing"},
        {"nodes = 2\nrouting = aloha\nmac = aloha\n", 2, "not a routing"},
        {"nodes = 2\nrouting = flood\nmac = flood\n", 3, "not a MAC"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nrouting.hops = 300\n", 4, "hop count"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nrouting.max_delay = 1 s\n", 4,
         "not a setting of flood"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nmac.hops = 1\n", 4, "not a setting of aloha"},
        {"nodes = 2\nrouting = meshtastic\nmac = meshtastic\nrouting.role = repeater\n", 4,
         "client, client_mute or router"},
        {"nodes = 2\nrouting = meshtastic\nmac = meshtastic\nrouting.hop_limit = 8\n", 4, "0 to 7"},
        {"nodes = 2\nrouting = meshtastic\nmac = meshtastic\nrouting.want_ack = maybe\n", 4,
         "yes or no"},
        {"nodes = 2\nrouting = meshtastic\nmac = meshtastic\nmac.cw_max = 16\n", 4, "0 to 15"},
        {"nodes = 2\nrouting = meshtastic\nmac = meshtastic\nmac.snr_max = loud\n", 4, "SNR in dB"},
        {"nodes = 2\nrouting = meshtastic\nmac = meshtastic\nrouting.cw_min = 9\n", 0,
         "routing meshtastic: cw_min is over cw_max"},
        {"nodes = 2\nrouting = meshtastic\nmac = meshtastic\nmac.snr_min = 10\n", 0,
         "mac meshtastic: snr_min is not below snr_max"},
        {"nodes = 2\nrouting = meshtastic\nmac = meshtastic\nmac.slot = 0 s\n", 0,
         "mac meshtastic: slot must be above 0"},
        {"nodes = 2\nrouting = meshtastic\nmac = meshtastic\nrouting.slot = 0 s\n", 0,
         "routing meshtastic: slot must be above 0"},
        {"nodes = 2\nrouting = meshtastic\nmac = meshtastic\nmac.slot = 1000000000 s\n"
         "mac.cw_min = 15\nmac.cw_max = 15\n",
         0, "mac meshtastic: slot is too long"},
        {"nodes = 2\nrouting = meshtastic\nmac = meshtastic\nrouting.processing = 2000000 h\n", 4,
         "too long for the clock"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nphy.fading = -1\n", 4, "dB from 0 to 1000"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nphy.pairwise = maybe\n", 4, "yes or no"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nphy.capture_anytime = 1\n", 4, "yes or no"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nphy.cad_margin = nan\n", 4, "dB from -1000"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nphy.cad_delay = 5\n", 4, "expected a time"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nchannel.model = okumura\n", 4,
         "log_distance, 3gpp_suburban or 3gpp_urban"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nchannel.freq = 0\n", 4, "MHz above 0"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nchannel.height = -1\n", 4, "metres above 0"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nplacement = file\n", 0, "positions is not set"},
        {"nodes = 2\nrouting = meshtastic\nmac = meshtastic\nmac.busy_chance = 2\n", 4,
         "fraction from 0 to 1"},
        {"nodes = 2\nrouting = meshtastic\nmac = meshtastic\nrouting.noise = loud\n", 4,
         "dBm from -1000 to 1000"},
        {"nodes = 2\nrouting = meshtastic\nmac = meshtastic\nrouting.ack_poll = 1\n", 4,
         "yes or no"},
        {"nodes = 2\nrouting = meshtastic\nmac = meshtastic\nrouting.cancel_late = on\n", 4,
         "yes or no"},
        {"nodes = 2\nrouting = flood\nmac = aloha\ntraffic.interval = 0 s\n", 4, "or none"},
        {"nodes = 2\nrouting = flood\nmac = aloha\ntraffic.send = 1 s, 0, all\n", 4,
         "as 30 s, 2, all, 40"},
        {"nodes = 2\nrouting = flood\nmac = aloha\ntraffic.send = 1 s, 0, 1, 40, 2\n", 4,
         "as 30 s, 2, all, 40"},
        {"nodes = 2\nrouting = flood\nmac = aloha\ntraffic.send = soon, 0, 1, 40\n", 4,
         "as 30 s, 2, all, 40"},
        {"nodes = 2\nrouting = flood\nmac = aloha\ntraffic.send = 1 s, 0, 1, 256\n", 4,
         "as 30 s, 2, all, 40"},
        {"nodes = 2\nrouting = flood\nmac = aloha\ntraffic.send = 1 s, 0, 2, 40\n", 0,
         "traffic.send 1 names a node past the 2"},
        {"nodes = 2\nrouting = flood\nmac = aloha\ntraffic.send = 1 s, 1, 1, 40\n", 0,
         "traffic.send 1 is from a node to itself"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nduration = 1 min\ndeadline = 1 min\n"
         "traffic.send = 2 min, 0, all, 40\n",
         0, "traffic.send 1 is at or after the end"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nwarmup = 1 min\n"
         "traffic.send = 30 s, 0, all, 40\n",
         0, "traffic.send 1 is during the warmup"},
        {"nodes = 2\nrouting = meshcore\nmac = meshcore\nrouting.relays = some\n", 4,
         "node numbers and ranges"},
        {"nodes = 2\nrouting = meshcore\nmac = meshcore\nrouting.hash_size = 4\n", 4,
         "1, 2 or 3 bytes"},
        {"nodes = 2\nrouting = meshcore\nmac = meshcore\nrouting.flood_max = 0\n", 4,
         "from 1 to 64"},
        {"nodes = 2\nrouting = meshcore\nmac = meshcore\nrouting.rx_delay_base = 21\n", 4,
         "from 0 to 20"},
        {"nodes = 2\nrouting = meshcore\nmac = meshcore\nrouting.tx_delay_factor = 3\n", 4,
         "from 0 to 2"},
        {"nodes = 2\nrouting = meshcore\nmac = meshcore\nrouting.estimate_cr = 5\n", 4,
         "radio, or a coding rate from 1 to 4"},
        {"nodes = 2\nrouting = meshcore\nmac = meshcore\nrouting.advert_interval = 0 s\n", 4,
         "or none"},
        {"nodes = 2\nrouting = meshcore\nmac = meshcore\nrouting.cancel_heard = yes\n", 4,
         "no, waiting or queued"},
        {"nodes = 2\nrouting = meshcore\nmac = meshcore\nmac.airtime_factor = -1\n", 4,
         "from 0 to 1000000"},
        {"nodes = 2\nrouting = meshcore\nmac = meshcore\nmac.latched_header = 0 s\n", 4, "or no"},
        {"nodes = 2\nrouting = flood\nmac = aloha\ntraffic.closed = sometimes\n", 4, "yes or no"},
        {"nodes = 2\nrouting = flood\nmac = aloha\ntraffic.peers = -1\n", 4, "count of nodes"},
        {"nodes = 3\nrouting = flood\nmac = aloha\ntraffic.peers = 3\n", 0,
         "traffic.peers is 3, more than the 2 other nodes"},
        {"nodes = 2\nrouting = flood\nmac = aloha\ntraffic.reply = 2\n", 4, "fraction"},
        {"nodes = 2\nrouting = flood\nmac = aloha\ntraffic.reply_delay = soon\n", 4,
         "expected a time"},
        {"nodes = 2\nrouting = flood\nmac = aloha\ntraffic.reply = 0.5\ntraffic.closed = yes\n", 0,
         "traffic.reply needs traffic.closed = no"},
        {"nodes = 2\n\n# a comment\njust some words\n", 4, "key = value"},
        {"nodes = 2\n = 3\n", 2, "no setting"},
        {"nodes = 2\nseed =   # nothing\n", 2, "seed has no value"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nradio.sf = 13\n", 4, "spreading factor"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nradio.bw = 0\n", 4, "bandwidth"},
        {"nodes = 2\nrouting = flood\nmac = aloha\ntraffic.len = 64..16\n", 4, "16..64"},
        {"nodes = 2\nrouting = flood\nmac = aloha\ntraffic.len = 256\n", 4, "up to 255"},
        {"nodes = 2\nrouting = flood\nmac = aloha\ntraffic.broadcast = 1.5\n", 4, "fraction"},
        {"nodes = 2\nrouting = flood\nmac = aloha\narea = 5000\n", 4, "5000 x 5000"},
        {"nodes = 2\nrouting = flood\nmac = aloha\narea = 0 x 10\n", 4, "5000 x 5000"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nspacing = nan\n", 4, "above 0"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nradio.tx_dbm = inf\n", 4, "dBm"},
        {"routing = flood\nmac = aloha\n", 0, "nodes is not set"},
        {"nodes = 2\nmac = aloha\n", 0, "routing is not set"},
        {"nodes = 2\nrouting = flood\n", 0, "mac is not set"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nduration = 3000000000 h\n", 4,
         "expected a time"},
        {"nodes = 2\nrouting = flood\nmac = aloha\narea = 1e300 x 1e300\n", 0, "billion"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nplacement = line\nspacing = 1e12\n", 0,
         "billion"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nchannel.decorrelation = 1e-300\n", 0, "billion"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nduration = 2000000 h\nwarmup = 1000000 h\n", 0,
         "too long"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct tsim_scenario s;
        struct tsim_scenario_error err;
        bool ok = tsim_scenario_parse(&s, cases[i].text, &err);
        CHECK(!ok);
        if (!ok && (err.line != cases[i].line || !strstr(err.message, cases[i].says))) {
            fprintf(stderr, "  case %zu: line %d: %s\n", i, err.line, err.message);
            CHECK(err.line == cases[i].line && strstr(err.message, cases[i].says));
        }
    }
}

static const char *line_text = "nodes = 6\nplacement = line\nspacing = 2000\nchannel.sigma = 0\n"
                               "routing = flood\nrouting.hops = 5\nmac = aloha\n"
                               "mac.max_delay = 500 ms\ntraffic.interval = 5 min\n"
                               "traffic.broadcast = 0.5\nwarmup = 10 s\nduration = 30 min\n"
                               "deadline = 20 s\n";

static bool same_delivery(const struct tsim_delivery *a, const struct tsim_delivery *b) {
    return a->messages == b->messages && a->refused == b->refused && a->wanted == b->wanted &&
           a->delivered == b->delivered && a->on_time == b->on_time &&
           a->latency_p50 == b->latency_p50 && a->latency_p95 == b->latency_p95 &&
           a->latency_max == b->latency_max;
}

static bool same_report(const struct tsim_report *a, const struct tsim_report *b) {
    for (int p = 0; p < TSIM_PURPOSE_COUNT; p++) {
        if (a->frames[p] != b->frames[p] || a->airtime_s[p] != b->airtime_s[p]) {
            return false;
        }
    }
    return a->elapsed == b->elapsed && same_delivery(&a->unicast, &b->unicast) &&
           same_delivery(&a->broadcast, &b->broadcast) && a->duty_max == b->duty_max &&
           a->duty_max_node == b->duty_max_node && a->rx_ok == b->rx_ok &&
           a->rx_lost == b->rx_lost && a->queue_dropped == b->queue_dropped;
}

/* Six nodes in a row that hear only their neighbours: whatever arrives was relayed there, and the
 * window measured is the traffic and the deadline, after the warmup. */
static const struct tsim_meshtastic_config *meshtastic_of(const struct tsim_scenario *s) {
    return (const struct tsim_meshtastic_config *)s->routing_config;
}

static const struct tsim_meshtastic_mac_config *meshtastic_mac_of(const struct tsim_scenario *s) {
    return (const struct tsim_meshtastic_mac_config *)s->mac_config;
}

/* Settings that bound each other are checked once all are in, so their order does not matter;
 * the slot follows the radio. */
static void meshtastic_settings_read_in_any_order(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, "nodes = 2\nrouting = meshtastic\nmac = meshtastic\n"
                    "radio.sf = 11\nradio.bw = 250000\n"
                    "routing.cw_min = 10\nrouting.cw_max = 12\n"
                    "mac.snr_max = -30\nmac.snr_min = -40\n"
                    "routing.role = router\nrouting.hop_limit = 7\nrouting.want_ack = no\n"
                    "routing.retries = 1\nrouting.processing = 1 s\n"
                    "routing.ack_duplicates = no\n"));
    const struct tsim_meshtastic_config *r = meshtastic_of(&s);
    const struct tsim_meshtastic_mac_config *m = meshtastic_mac_of(&s);
    CHECK(r->window.cw_min == 10 && r->window.cw_max == 12);
    CHECK(m->snr_min_db == -40 && m->snr_max_db == -30);
    CHECK(r->role == TSIM_MESHTASTIC_ROUTER && r->hop_limit == 7 && !r->want_ack);
    CHECK(r->retries == 1 && r->processing == TSIM_S(1) && !r->ack_duplicates);
    CHECK(r->lora.sf == 11 && r->lora.bw_hz == 250000);
    CHECK_EQ_I64(r->window.slot, tsim_meshtastic_slot(&r->lora));
    CHECK_EQ_I64(m->window.slot, r->window.slot);
    CHECK(m->window.cw_min == 3 && m->window.cw_max == 8);

    CHECK(parse(&s, "nodes = 2\nrouting = meshtastic\nmac = meshtastic\nmac.slot = 50 ms\n"));
    CHECK_EQ_I64(meshtastic_mac_of(&s)->window.slot, TSIM_MS(50));
}

static void a_meshtastic_run_floods_a_line(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, "nodes = 6\nplacement = line\nspacing = 2000\nchannel.sigma = 0\n"
                    "routing = meshtastic\nmac = meshtastic\n"
                    "traffic.interval = 1 min\ntraffic.broadcast = 1\nduration = 30 min\n"));
    struct tsim_report rep;
    CHECK(tsim_scenario_run(&s, &rep));
    CHECK(rep.broadcast.messages > 100);
    CHECK(rep.broadcast.on_time > rep.broadcast.wanted / 2);
    CHECK(rep.frames[TSIM_PURPOSE_RELAY] > 0);
}

static void a_run_relays_across_its_map(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, line_text));
    struct tsim_report rep;
    CHECK(tsim_scenario_run(&s, &rep));
    CHECK_EQ_I64(rep.elapsed, TSIM_S(30 * 60 + 20));
    CHECK_EQ_I64(rep.warmup, TSIM_S(10));
    CHECK(rep.warmup_airtime_s == 0); /* flooding sends nothing until there is traffic */
    CHECK(rep.routes == -1 && rep.routes_reach == -1); /* and keeps no routes */
    CHECK(rep.unicast.messages + rep.broadcast.messages > 20);
    CHECK(rep.frames[TSIM_PURPOSE_RELAY] > 0);
    CHECK(rep.broadcast.delivered > rep.broadcast.wanted / 2);
    CHECK(rep.on_time_per_airtime_s > 0);
}

/* Candidate 3 announces through its warmup: what that cost is reported apart from the window, which
 * it leaves every node on the line holding a route to every other, and each route reaching. */
static void a_warmup_is_reported_apart(void) {
    struct tsim_scenario s;
    const char *text = "nodes = 6\nplacement = line\nspacing = 2000\nchannel.sigma = 0\n"
                       "routing = distvec\nmac = meshcore\ntraffic.interval = 24 h\n"
                       "warmup = %s\nduration = 10 min\n";
    char buf[512];
    snprintf(buf, sizeof buf, text, "30 min");
    CHECK(parse(&s, buf));
    struct tsim_report settled;
    CHECK(tsim_scenario_run(&s, &settled));
    CHECK_EQ_I64(settled.warmup, TSIM_S(30 * 60));
    CHECK(settled.warmup_airtime_s > 0);
    CHECK(settled.routes == 1.0 && settled.routes_reach == 1.0);
    /* The window's airtime is what a run without the warmup would add after it. */
    snprintf(buf, sizeof buf, text, "0 s");
    CHECK(parse(&s, buf));
    struct tsim_report cold;
    CHECK(tsim_scenario_run(&s, &cold));
    CHECK_EQ_I64(cold.warmup, 0);
    CHECK(cold.warmup_airtime_s == 0);
    CHECK(cold.routes == 0.0 && cold.routes_reach == 0.0);
    CHECK(settled.airtime_total_s < cold.airtime_total_s); /* settled, it announces less */
}

/* Peers and answers reach the traffic: four nodes with one peer each, every unicast answered. */
static void a_run_talks_to_its_peers_and_answers(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, "nodes = 4\nrouting = flood\nmac = aloha\ntraffic.broadcast = 0\n"
                    "traffic.peers = 1\ntraffic.reply = 1\ntraffic.reply_delay = 5 s\n"
                    "traffic.interval = 10 min\nduration = 2 h\n"));
    CHECK(s.peers == 1 && s.reply == 1.0 && s.reply_delay == TSIM_S(5));
    struct tsim_report rep;
    CHECK(tsim_scenario_run(&s, &rep));
    /* 48 expected, and as many answers, less those past the end. */
    CHECK(rep.unicast.messages > 60 && rep.unicast.messages < 140);
    CHECK(parse(&s, "nodes = 4\nrouting = flood\nmac = aloha\n"));
    CHECK(s.peers == 0 && s.reply == 0.0 && s.reply_delay == TSIM_S(120));
}

/* A line 2 km apart at 20 dBm: each node decodes those two hops away, not three, so the ends
 * have two links and the middle four. At 0 dBm a neighbour is out of reach. */
static void a_run_reports_its_links(void) {
    struct tsim_scenario s;
    const char *text = "nodes = 6\nplacement = line\nspacing = 2000\nchannel.sigma = 0\n"
                       "routing = flood\nmac = aloha\nradio.tx_dbm = %d\nduration = 1 min\n";
    char buf[256];
    snprintf(buf, sizeof buf, text, 20);
    CHECK(parse(&s, buf));
    struct tsim_report rep;
    CHECK(tsim_scenario_run(&s, &rep));
    CHECK(rep.links.degree_mean == 3.0);
    CHECK_EQ_I64(rep.links.degree_min, 2);
    CHECK_EQ_I64(rep.links.degree_max, 4);
    CHECK_EQ_I64(rep.links.component_max, 6);
    snprintf(buf, sizeof buf, text, 0);
    CHECK(parse(&s, buf));
    CHECK(tsim_scenario_run(&s, &rep));
    CHECK(rep.links.degree_mean == 0.0);
    CHECK_EQ_I64(rep.links.component_max, 1);
}

/* Forty radios at 1 Hz, SF12 and a 65535-symbol preamble send frames years long, and between
 * them spend more airtime than a tsim_time holds. The report adds it up regardless. */
static void airtime_past_what_a_time_holds_still_adds_up(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, "nodes = 40\nrouting = flood\nmac = aloha\narea = 2000 x 2000\n"
                    "radio.bw = 1\nradio.sf = 12\nradio.preamble = 65535\n"
                    "traffic.interval = 24 h\nduration = 70080 h\ndeadline = 1 h\n"));
    struct tsim_report rep;
    CHECK(tsim_scenario_run(&s, &rep));
    CHECK(rep.airtime_total_s > (double)INT64_MAX / 1e9);
    CHECK(rep.duty_max > 0 && rep.duty_max <= 1.0);
    CHECK(rep.duty_mean > 0 && rep.duty_mean <= 1.0);
}

/* The settings that reproduce LoRaSim's and Meshtasticator's simplifications. */
static void compatibility_settings_read(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, "nodes = 2\nrouting = meshtastic\nmac = meshtastic\n"
                    "phy.fading = 2\nphy.pairwise = yes\nphy.capture_anytime = yes\n"
                    "phy.cad_margin = 3\nphy.cad_delay = 28 ms\n"
                    "channel.model = 3gpp_suburban\nchannel.freq = 908.75\nchannel.height = 1.5\n"
                    "routing.noise = -119.25\nmac.busy_chance = 0.1\n"
                    "routing.ack_poll = yes\nrouting.cancel_late = yes\ntraffic.closed = yes\n"
                    "placement = file\npositions = here.positions\n"));
    CHECK(s.net.phy.fading_db == 2 && s.net.phy.pairwise && s.net.phy.capture_anytime);
    CHECK(s.net.phy.cad_margin_db == 3 && s.net.phy.cad_delay == TSIM_MS(28));
    CHECK(s.channel.model == TSIM_PATH_3GPP_SUBURBAN);
    CHECK(s.channel.freq_mhz == 908.75 && s.channel.height_m == 1.5);
    CHECK(meshtastic_of(&s)->noise_dbm == -119.25);
    CHECK(meshtastic_mac_of(&s)->busy_chance == 0.1);
    CHECK(meshtastic_of(&s)->ack_poll && meshtastic_of(&s)->cancel_late && s.closed);
    CHECK(s.placement == TSIM_PLACEMENT_FILE && strcmp(s.positions_file, "here.positions") == 0);
    CHECK(s.positions == NULL);

    CHECK(parse(&s,
                "nodes = 2\nrouting = meshtastic\nmac = meshtastic\nchannel.model = 3gpp_urban\n"));
    CHECK(s.channel.model == TSIM_PATH_3GPP_URBAN && !s.net.phy.pairwise);
    CHECK(isnan(meshtastic_of(&s)->noise_dbm));
    CHECK(!meshtastic_of(&s)->ack_poll && !meshtastic_of(&s)->cancel_late && !s.closed);
}

static const struct tsim_meshcore_config *meshcore_of(const struct tsim_scenario *s) {
    return (const struct tsim_meshcore_config *)s->routing_config;
}

static void meshcore_settings_read(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, "nodes = 2\nrouting = meshcore\nmac = meshcore\n"));
    const struct tsim_meshcore_config *r = meshcore_of(&s);
    CHECK(strcmp(r->relays, "all") == 0 && r->hash_size == 1 && r->scoped);
    CHECK(r->flood_max == 64 && r->rx_delay_base == 0 && r->tx_delay_factor == 0.5);
    CHECK(r->direct_tx_delay_factor == 0.3 && r->retries == 3);
    CHECK(r->cancel_heard == TSIM_MESHCORE_CANCEL_NO);
    CHECK(r->advert_interval == TSIM_S(120) && r->estimate_cr == 0);
    CHECK(((const struct tsim_meshcore_mac_config *)s.mac_config)->airtime_factor == 1.0);
    CHECK(((const struct tsim_meshcore_mac_config *)s.mac_config)->latched_header == 0);

    CHECK(parse(&s, "nodes = 2\nrouting = meshcore\nmac = meshcore\nradio.sf = 8\n"
                    "routing.relays = 0-45,50\nrouting.hash_size = 2\nrouting.scoped = no\n"
                    "routing.flood_max = 8\nrouting.rx_delay_base = 10\n"
                    "routing.tx_delay_factor = 1\nrouting.direct_tx_delay_factor = 0\n"
                    "routing.retries = 0\nrouting.advert_interval = none\n"
                    "routing.cancel_heard = waiting\nrouting.estimate_cr = 1\n"
                    "mac.airtime_factor = 9\nmac.latched_header = 3934 ms\n"));
    r = meshcore_of(&s);
    CHECK(strcmp(r->relays, "0-45,50") == 0 && r->hash_size == 2 && !r->scoped);
    CHECK(r->flood_max == 8 && r->rx_delay_base == 10 && r->tx_delay_factor == 1);
    CHECK(r->direct_tx_delay_factor == 0 && r->retries == 0);
    CHECK(r->cancel_heard == TSIM_MESHCORE_CANCEL_WAITING);
    CHECK(r->advert_interval == 0 && r->estimate_cr == 1 && r->lora.sf == 8);
    CHECK(((const struct tsim_meshcore_mac_config *)s.mac_config)->airtime_factor == 9.0);
    CHECK(((const struct tsim_meshcore_mac_config *)s.mac_config)->latched_header == TSIM_MS(3934));
    CHECK(parse(&s, "nodes = 2\nrouting = meshcore\nmac = meshcore\n"
                    "mac.latched_header = 1 s\nmac.latched_header = no\n"));
    CHECK(((const struct tsim_meshcore_mac_config *)s.mac_config)->latched_header == 0);
    CHECK(parse(&s, "nodes = 2\nrouting = meshcore\nmac = meshcore\n"
                    "routing.estimate_cr = 2\nrouting.estimate_cr = radio\n"));
    CHECK(meshcore_of(&s)->estimate_cr == 0);
}

static void sends_add_up_and_interval_none_stops_the_process(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, "nodes = 3\nrouting = flood\nmac = aloha\ntraffic.interval = none\n"
                    "traffic.send = 30 s, 2, all, 40\ntraffic.send = 1.5 min , 0 , 1 , 7\n"));
    CHECK_EQ_I64(s.interval, 0);
    CHECK_EQ_U64(s.send_count, 2);
    CHECK(s.sends[0].at == TSIM_S(30) && s.sends[0].src == 2 && s.sends[0].dst == TSIM_BROADCAST &&
          s.sends[0].len == 40);
    CHECK(s.sends[1].at == TSIM_S(90) && s.sends[1].src == 0 && s.sends[1].dst == 1 &&
          s.sends[1].len == 7);
    struct tsim_report rep;
    CHECK(tsim_scenario_run(&s, &rep));
    CHECK_EQ_U64(rep.broadcast.messages, 1);
    CHECK_EQ_U64(rep.unicast.messages, 1);

    char text[4096];
    int n = snprintf(text, sizeof text, "nodes = 2\nrouting = flood\nmac = aloha\n");
    for (int i = 0; i < TSIM_SENDS_MAX; i++) {
        n += snprintf(text + n, sizeof text - (size_t)n, "traffic.send = 1 s, 0, 1, 1\n");
    }
    CHECK(parse(&s, text));
    snprintf(text + n, sizeof text - (size_t)n, "traffic.send = 1 s, 0, 1, 1\n");
    struct tsim_scenario_error err;
    CHECK(!tsim_scenario_parse(&s, text, &err) && strstr(err.message, "64 a scenario can hold"));
}

static void links_read_from_text(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, "nodes = 3\nrouting = flood\nmac = aloha\nlinks = l\n"));
    CHECK(strcmp(s.links_file, "l") == 0 && s.links == NULL);
    struct tsim_link *links;
    size_t count;
    struct tsim_scenario_error err;
    CHECK(tsim_scenario_read_links(&s, "# a b loss\n0 1 120.5\n\n 2 0  130 131.25  # back\n",
                                   &links, &count, &err));
    CHECK_EQ_U64(count, 4);
    /* In order: 0 to 1, 0 to 2, 1 to 0, 2 to 0. */
    CHECK(links[0].from == 0 && links[0].to == 1 && links[0].loss_db == 120.5);
    CHECK(links[1].from == 0 && links[1].to == 2 && links[1].loss_db == 131.25);
    CHECK(links[2].from == 1 && links[2].to == 0 && links[2].loss_db == 120.5);
    CHECK(links[3].from == 2 && links[3].to == 0 && links[3].loss_db == 130);
    free(links);

    static const struct {
        const char *text;
        int line;
        const char *says;
    } bad[] = {
        {"", 0, "there are no links"},
        {"# nothing\n\n", 0, "there are no links"},
        {"0 1 100\n0 3 100\n", 2, "two node numbers below 3"},
        {"0 1 100\n-1 2 100\n", 2, "two node numbers below 3"},
        {"0 1\n", 1, "a loss in dB"},
        {"0 1 nan\n", 1, "a loss in dB"},
        {"0 1 1e4\n", 1, "a loss in dB"},
        {"0 1 100 100 100\n", 1, "at most a second loss"},
        {"0 1 100 x\n", 1, "at most a second loss"},
        {"1 1 100\n", 1, "linked to itself"},
        {"0 1 100\n1 0 100\n", 0, "between 0 and 1 is given twice"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        bool ok = tsim_scenario_read_links(&s, bad[i].text, &links, &count, &err);
        CHECK(!ok && links == NULL && count == 0);
        if (!ok && (err.line != bad[i].line || !strstr(err.message, bad[i].says))) {
            fprintf(stderr, "  case %zu: line %d: %s\n", i, err.line, err.message);
            CHECK(false);
        }
    }
}

/* Losses from a file: nodes 0 and 2 hear only node 1, which relays between them; and a scenario
 * that names a links file does not run without its links. */
static void a_run_with_links_uses_them(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, "nodes = 3\nrouting = flood\nmac = aloha\nlinks = l\n"
                    "traffic.interval = none\ntraffic.send = 1 s, 0, all, 20\nduration = 10 s\n"));
    struct tsim_report rep;
    CHECK(!tsim_scenario_run(&s, &rep));
    struct tsim_link *links;
    struct tsim_scenario_error err;
    CHECK(tsim_scenario_read_links(&s, "0 1 100\n1 2 100\n", &links, &s.link_count, &err));
    s.links = links;
    CHECK(tsim_scenario_run(&s, &rep));
    CHECK_EQ_U64(rep.broadcast.delivered, 2);
    CHECK_EQ_U64(rep.frames[TSIM_PURPOSE_RELAY], 2); /* node 1's, and node 2's, heard by node 1 */

    /* Placed from a file it never read, it runs on its links all the same. */
    struct tsim_scenario placed;
    CHECK(parse(&placed, "nodes = 3\nrouting = flood\nmac = aloha\nlinks = l\n"
                         "placement = file\ntraffic.interval = none\n"
                         "traffic.send = 1 s, 0, all, 20\nduration = 10 s\n"));
    placed.links = links;
    placed.link_count = s.link_count;
    struct tsim_report again;
    CHECK(tsim_scenario_run(&placed, &again));
    CHECK_EQ_U64(again.broadcast.delivered, 2);
    free(links);
}

static void positions_read_from_text(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, "nodes = 3\nrouting = flood\nmac = aloha\nplacement = file\npositions = p\n"));
    struct tsim_pos pos[3];
    struct tsim_scenario_error err;
    CHECK(tsim_scenario_read_positions(&s, "# x y\n0 0\n\n 1500.5, -20   # a comment\n3e3 4e3", pos,
                                       &err));
    CHECK(pos[0].x == 0 && pos[0].y == 0);
    CHECK(pos[1].x == 1500.5 && pos[1].y == -20);
    CHECK(pos[2].x == 3000 && pos[2].y == 4000);

    static const struct {
        const char *text;
        int line;
        const char *says;
    } bad[] = {
        {"0 0\n1 1\n", 0, "2 positions for 3 nodes"},
        {"0 0\n1 1\n2 2\n3 3\n", 4, "more positions than the 3 nodes"},
        {"0 0\n1\n2 2\n", 2, "x y in metres"},
        {"0 0\n1 1 1\n2 2\n", 2, "x y in metres"},
        {"0 0\nnorth south\n2 2\n", 2, "x y in metres"},
        {"0 0\ninf 1\n2 2\n", 2, "billion"},
        {"0 0\nnan 1\n2 2\n", 2, "billion"},
        {"0 0\n1 1e12\n2 2\n", 2, "billion"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        bool ok = tsim_scenario_read_positions(&s, bad[i].text, pos, &err);
        CHECK(!ok);
        if (!ok && (err.line != bad[i].line || !strstr(err.message, bad[i].says))) {
            fprintf(stderr, "  case %zu: line %d: %s\n", i, err.line, err.message);
            CHECK(false);
        }
    }
}

/* A scenario placed from a file runs where its positions say, and not without them. */
static void a_run_placed_from_a_file_uses_its_positions(void) {
    struct tsim_scenario s;
    const char *line =
        "nodes = 6\nplacement = line\nspacing = 2000\nchannel.sigma = 0\n"
        "routing = flood\nmac = aloha\ntraffic.interval = 5 min\nduration = 30 min\n";
    CHECK(parse(&s, line));
    struct tsim_report want, got;
    CHECK(tsim_scenario_run(&s, &want));

    char text[2048];
    snprintf(text, sizeof text, "%s placement = file\npositions = line.positions\n", line);
    CHECK(parse(&s, text));
    CHECK(!tsim_scenario_run(&s, &got));
    struct tsim_pos pos[6];
    struct tsim_scenario_error err;
    CHECK(tsim_scenario_read_positions(&s, "0 0\n2000 0\n4000 0\n6000 0\n8000 0\n10000 0\n", pos,
                                       &err));
    s.positions = pos;
    CHECK(tsim_scenario_run(&s, &got));
    CHECK(same_report(&want, &got));
}

static void a_run_repeats_with_its_seed(void) {
    struct tsim_scenario s;
    struct tsim_report a, b, c;
    CHECK(parse(&s, line_text));
    CHECK(tsim_scenario_run(&s, &a));
    CHECK(tsim_scenario_run(&s, &b));
    CHECK(same_report(&a, &b));
    s.seed = 2;
    CHECK(tsim_scenario_run(&s, &c));
    CHECK(!same_report(&a, &c));
}

int main(void) {
    RUN(the_documented_example_parses);
    RUN(every_value_kind_reads);
    RUN(plugin_settings_may_come_first);
    RUN(a_later_setting_wins);
    RUN(unset_settings_take_their_defaults);
    RUN(problems_say_where_they_are);
    RUN(meshtastic_settings_read_in_any_order);
    RUN(a_run_relays_across_its_map);
    RUN(a_warmup_is_reported_apart);
    RUN(a_run_reports_its_links);
    RUN(a_run_talks_to_its_peers_and_answers);
    RUN(a_meshtastic_run_floods_a_line);
    RUN(a_run_repeats_with_its_seed);
    RUN(compatibility_settings_read);
    RUN(positions_read_from_text);
    RUN(meshcore_settings_read);
    RUN(sends_add_up_and_interval_none_stops_the_process);
    RUN(links_read_from_text);
    RUN(a_run_with_links_uses_them);
    RUN(a_run_placed_from_a_file_uses_its_positions);
    RUN(airtime_past_what_a_time_holds_still_adds_up);
    return CHECK_DONE();
}
