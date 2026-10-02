#ifndef TSIM_SCENARIO_H
#define TSIM_SCENARIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tsim/channel.h"
#include "tsim/metrics.h"
#include "tsim/net.h"
#include "tsim/time.h"
#include "tsim/traffic.h"

/* A run, written down: where the nodes stand, how the air behaves, which protocol runs, what it is
 * asked to carry and for how long. A scenario file is text, one setting per line:
 *
 *     # 200 nodes over 5 km, naive flooding
 *     nodes     = 200
 *     placement = uniform
 *     area      = 5000 x 5000      # metres
 *     routing   = flood
 *     routing.hops = 3
 *     mac       = aloha
 *     mac.max_delay = 2 s
 *     traffic.interval = 15 min
 *     duration  = 1 h
 *
 * A '#' starts a comment. A time takes a unit (ns, us, ms, s, min, h), so a bare number is never
 * read in the wrong one. A setting given twice takes the later value, which is how a run is varied
 * from the command line without editing the file. routing.* and mac.* are the chosen plugin's own
 * settings, and may come before or after the line that chooses it; a plugin's radio settings
 * start from the radio.* ones. Every setting has a default except nodes, routing and mac; the
 * defaults are listed in docs/scenarios.md.
 *
 * The run's clock: a warmup with no traffic, for protocols that announce themselves before they
 * can route; `duration` of traffic; then `deadline` more with none, so the last messages have as
 * long to arrive as the first. Every airtime and duty cycle counts all three. */

enum tsim_placement {
    TSIM_PLACEMENT_UNIFORM,
    TSIM_PLACEMENT_GRID,
    TSIM_PLACEMENT_LINE,
    TSIM_PLACEMENT_FILE, /* where a positions file says */
};

/* What a plugin's radio settings start from. */
struct tsim_radio {
    uint16_t channel;
    struct tsim_lora lora;
    double tx_dbm;
};

/* A routing or MAC the scenario format knows by name. */
struct tsim_plugin;

/* Room for any plugin's configuration. */
#define TSIM_PLUGIN_CONFIG_MAX 256

struct tsim_scenario {
    uint64_t seed;
    uint32_t nodes;
    enum tsim_placement placement;
    double width_m; /* uniform */
    double height_m;
    double spacing_m; /* grid and line */
    /* file: the path as written, and the positions the driver read from it, `nodes` of them, with
     * tsim_scenario_read_positions(). The scenario does not own them. */
    char positions_file[256];
    const struct tsim_pos *positions;

    struct tsim_channel_params channel; /* its seed is the run's */
    struct tsim_net_params net;         /* its listen settings and seed are the run's */
    struct tsim_radio radio;

    const struct tsim_plugin *routing;
    const struct tsim_plugin *mac;
    _Alignas(max_align_t) unsigned char routing_config[TSIM_PLUGIN_CONFIG_MAX];
    _Alignas(max_align_t) unsigned char mac_config[TSIM_PLUGIN_CONFIG_MAX];

    tsim_time interval; /* traffic: see tsim/traffic.h */
    uint32_t len_min;
    uint32_t len_max;
    double broadcast;
    bool closed;

    tsim_time warmup;
    tsim_time duration;
    tsim_time deadline;
};

struct tsim_scenario_error {
    int line; /* 0 for a problem with the scenario as a whole */
    char message[160];
};

/* Reads a scenario from text. Returns false, and says where and why in `err`, for an unknown
 * setting, a value that does not parse or is out of range, or a scenario missing nodes, routing
 * or mac. */
bool tsim_scenario_parse(struct tsim_scenario *scenario, const char *text,
                         struct tsim_scenario_error *err);

/* Reads a positions file for a scenario placed from one: a node per line, in node order, as its
 * x and y in metres separated by spaces or a comma. A '#' starts a comment, and blank lines are
 * skipped. Returns false, saying which line and why, unless there are exactly `nodes` positions,
 * each finite and within a billion channel.decorrelation cells of the origin. */
bool tsim_scenario_read_positions(const struct tsim_scenario *scenario, const char *text,
                                  struct tsim_pos *out, struct tsim_scenario_error *err);

/* The plugins' names, for messages and reports. */
const char *tsim_scenario_routing_name(const struct tsim_scenario *scenario);
const char *tsim_scenario_mac_name(const struct tsim_scenario *scenario);

/* Runs the scenario from start to finish and reports on it. Returns false if it could not be set
 * up: memory ran out, a plugin refused its configuration, or it is placed from a file and has
 * no positions. */
bool tsim_scenario_run(const struct tsim_scenario *scenario, struct tsim_report *report);

#endif
