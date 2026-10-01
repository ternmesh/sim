#include "tsim/scenario.h"

#include <string.h>

#include "tsim/baseline.h"

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
        {"nodes = 2\nplacement = circle\nrouting = flood\nmac = aloha\n", 2, "uniform, grid"},
        {"nodes = 2\nrouting = ospf\nmac = aloha\n", 2, "not a routing"},
        {"nodes = 2\nrouting = aloha\nmac = aloha\n", 2, "not a routing"},
        {"nodes = 2\nrouting = flood\nmac = flood\n", 3, "not a MAC"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nrouting.hops = 300\n", 4, "hop count"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nrouting.max_delay = 1 s\n", 4,
         "not a setting of flood"},
        {"nodes = 2\nrouting = flood\nmac = aloha\nmac.hops = 1\n", 4, "not a setting of aloha"},
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
        if (a->frames[p] != b->frames[p] || a->airtime[p] != b->airtime[p]) {
            return false;
        }
    }
    return a->elapsed == b->elapsed && same_delivery(&a->unicast, &b->unicast) &&
           same_delivery(&a->broadcast, &b->broadcast) && a->duty_max == b->duty_max &&
           a->duty_max_node == b->duty_max_node && a->rx_ok == b->rx_ok &&
           a->rx_lost == b->rx_lost && a->queue_dropped == b->queue_dropped;
}

/* Six nodes in a row that hear only their neighbours: whatever arrives was relayed there, and the
 * run's clock is the warmup, the traffic and the deadline. */
static void a_run_relays_across_its_map(void) {
    struct tsim_scenario s;
    CHECK(parse(&s, line_text));
    struct tsim_report rep;
    CHECK(tsim_scenario_run(&s, &rep));
    CHECK_EQ_I64(rep.elapsed, TSIM_S(10 + 30 * 60 + 20));
    CHECK(rep.unicast.messages + rep.broadcast.messages > 20);
    CHECK(rep.frames[TSIM_PURPOSE_RELAY] > 0);
    CHECK(rep.broadcast.delivered > rep.broadcast.wanted / 2);
    CHECK(rep.on_time_per_airtime_s > 0);
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
    RUN(a_run_relays_across_its_map);
    RUN(a_run_repeats_with_its_seed);
    return CHECK_DONE();
}
