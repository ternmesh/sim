/* tsim: runs a scenario file and prints its report as one JSON object.
 *
 *     tsim [-s key=value]... scenario.tsim
 *
 * A scenario placed from a file names it with `positions`, and one whose losses come from a file
 * names it with `links`; a relative path is from the scenario file's directory.
 *
 * Each -s is read as a line appended to the file, so it overrides the file's setting; a seed sweep
 * is a loop over -s seed=N. The report goes to stdout and is the same on every run of the same
 * scenario and seed; how long the run took goes to stderr, because it is not. */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tsim/scenario.h"

#include "json.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    while (buf) {
        len += fread(buf + len, 1, cap - len - 1, f);
        if (len < cap - 1) {
            break;
        }
        char *grown = realloc(buf, cap * 2);
        if (!grown) {
            free(buf);
            buf = NULL;
            break;
        }
        buf = grown;
        cap *= 2;
    }
    if (buf && ferror(f)) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[len] = '\0';
    }
    return buf;
}

/* The file, then each override on a line of its own. */
static char *with_overrides(char *text, char **sets, int count) {
    size_t len = strlen(text) + 2;
    for (int i = 0; i < count; i++) {
        len += strlen(sets[i]) + 1;
    }
    char *all = malloc(len);
    if (!all) {
        return NULL;
    }
    strcpy(all, text);
    strcat(all, "\n");
    for (int i = 0; i < count; i++) {
        strcat(all, sets[i]);
        strcat(all, "\n");
    }
    return all;
}

static double seconds(tsim_time t) { return (double)t / 1e9; }

static void print_delivery(const char *name, const struct tsim_delivery *d, const char *tail) {
    printf("  \"%s\": {\"messages\": %" PRIu64 ", \"refused\": %" PRIu64 ", \"wanted\": %" PRIu64
           ", \"delivered\": %" PRIu64 ", \"on_time\": %" PRIu64
           ", \"latency_p50_s\": %.6f, \"latency_p95_s\": %.6f, \"latency_max_s\": %.6f}%s\n",
           name, d->messages, d->refused, d->wanted, d->delivered, d->on_time,
           seconds(d->latency_p50), seconds(d->latency_p95), seconds(d->latency_max), tail);
}

static void print_report(const char *path, const struct tsim_scenario *s,
                         const struct tsim_report *r) {
    static const char *purposes[TSIM_PURPOSE_COUNT] = {"data", "relay", "control", "announce"};
    printf("{\n");
    printf("  \"scenario\": ");
    json_string(stdout, path);
    printf(",\n");
    printf("  \"seed\": %" PRIu64 ", \"nodes\": %" PRIu32
           ", \"routing\": \"%s\", \"mac\": \"%s\",\n",
           s->seed, s->nodes, tsim_scenario_routing_name(s), tsim_scenario_mac_name(s));
    printf("  \"elapsed_s\": %.6f, \"deadline_s\": %.6f,\n", seconds(r->elapsed),
           seconds(r->deadline));
    printf("  \"on_time_per_airtime_s\": %.6g,\n", r->on_time_per_airtime_s);
    printf("  \"duty_max\": %.6g, \"duty_max_node\": %" PRIu32 ", \"duty_mean\": %.6g,\n",
           r->duty_max, r->duty_max_node, r->duty_mean);
    print_delivery("unicast", &r->unicast, ",");
    print_delivery("broadcast", &r->broadcast, ",");
    printf("  \"airtime_s\": {\"total\": %.6f", r->airtime_total_s);
    for (int p = 0; p < TSIM_PURPOSE_COUNT; p++) {
        printf(", \"%s\": %.6f", purposes[p], r->airtime_s[p]);
    }
    printf("},\n  \"frames\": {");
    for (int p = 0; p < TSIM_PURPOSE_COUNT; p++) {
        printf("%s\"%s\": %" PRIu64, p ? ", " : "", purposes[p], r->frames[p]);
    }
    printf("},\n");
    printf("  \"queue_dropped\": %" PRIu64 ", \"rx_ok\": %" PRIu64 ", \"rx_lost\": %" PRIu64
           ", \"rx_preempted\": %" PRIu64 ", \"rx_aborted\": %" PRIu64 ", \"rx_missed\": %" PRIu64
           "\n",
           r->queue_dropped, r->rx_ok, r->rx_lost, r->rx_preempted, r->rx_aborted, r->rx_missed);
    printf("}\n");
}

static int usage(void) {
    fprintf(stderr, "usage: tsim [-s key=value]... scenario.tsim\n");
    return 2;
}

static int count_lines(const char *text) {
    int lines = 1;
    for (const char *c = text; *c; c++) {
        lines += *c == '\n';
    }
    return lines;
}

/* Parses the file and its overrides, saying where any problem is. */
static bool parse(struct tsim_scenario *scenario, const char *path, char **sets, int set_count) {
    char *text = read_file(path);
    if (!text) {
        fprintf(stderr, "tsim: cannot read %s\n", path);
        return false;
    }
    int file_lines = count_lines(text);
    char *all = with_overrides(text, sets, set_count);
    free(text);
    if (!all) {
        fprintf(stderr, "tsim: out of memory\n");
        return false;
    }
    struct tsim_scenario_error err;
    bool ok = tsim_scenario_parse(scenario, all, &err);
    free(all);
    if (ok) {
        return true;
    }
    if (err.line > file_lines) {
        fprintf(stderr, "-s %s: %s\n", sets[err.line - file_lines - 1], err.message);
    } else if (err.line > 0) {
        fprintf(stderr, "%s:%d: %s\n", path, err.line, err.message);
    } else {
        fprintf(stderr, "%s: %s\n", path, err.message);
    }
    return false;
}

/* Reads a file a scenario names, which a relative path finds beside the scenario file. Returns
 * its text and its path, or NULL having said why. */
static char *read_beside(const char *scenario_path, const char *file, const char *what, char *path,
                         size_t size) {
    const char *slash = strrchr(scenario_path, '/');
    int len =
        file[0] == '/' || !slash
            ? snprintf(path, size, "%s", file)
            : snprintf(path, size, "%.*s/%s", (int)(slash - scenario_path), scenario_path, file);
    if (len < 0 || (size_t)len >= size) {
        fprintf(stderr, "%s: the %s path is too long\n", scenario_path, what);
        return NULL;
    }
    char *text = read_file(path);
    if (!text) {
        fprintf(stderr, "%s: cannot read %s from %s\n", scenario_path, what, path);
    }
    return text;
}

static void file_error(const char *path, const struct tsim_scenario_error *err) {
    if (err->line > 0) {
        fprintf(stderr, "%s:%d: %s\n", path, err->line, err->message);
    } else {
        fprintf(stderr, "%s: %s\n", path, err->message);
    }
}

/* Reads the positions of a scenario placed from a file. Returns them, for the scenario to point
 * at, or NULL having said why. */
static struct tsim_pos *load_positions(struct tsim_scenario *s, const char *scenario_path) {
    char path[4096];
    char *text = read_beside(scenario_path, s->positions_file, "positions", path, sizeof path);
    if (!text) {
        return NULL;
    }
    struct tsim_pos *pos = malloc(s->nodes * sizeof *pos);
    struct tsim_scenario_error err;
    if (!pos) {
        fprintf(stderr, "tsim: out of memory\n");
    } else if (!tsim_scenario_read_positions(s, text, pos, &err)) {
        file_error(path, &err);
        free(pos);
        pos = NULL;
    }
    free(text);
    s->positions = pos;
    return pos;
}

/* Reads the links of a scenario whose losses come from a file. Returns them, for the scenario to
 * point at, or NULL having said why. */
static struct tsim_link *load_links(struct tsim_scenario *s, const char *scenario_path) {
    char path[4096];
    char *text = read_beside(scenario_path, s->links_file, "links", path, sizeof path);
    if (!text) {
        return NULL;
    }
    struct tsim_link *links;
    struct tsim_scenario_error err;
    if (!tsim_scenario_read_links(s, text, &links, &s->link_count, &err)) {
        file_error(path, &err);
        links = NULL;
    }
    free(text);
    s->links = links;
    return links;
}

int main(int argc, char **argv) {
    char **sets = calloc((size_t)argc, sizeof *sets);
    int set_count = 0;
    const char *path = NULL;
    if (!sets) {
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            sets[set_count++] = argv[++i];
        } else if (argv[i][0] == '-' || path) {
            path = NULL;
            break;
        } else {
            path = argv[i];
        }
    }
    if (!path) {
        free(sets);
        return usage();
    }
    struct tsim_scenario scenario;
    bool parsed = parse(&scenario, path, sets, set_count);
    free(sets);
    if (!parsed) {
        return 1;
    }
    struct tsim_pos *positions = NULL;
    struct tsim_link *links = NULL;
    if ((scenario.placement == TSIM_PLACEMENT_FILE &&
         !(positions = load_positions(&scenario, path))) ||
        (scenario.links_file[0] && !(links = load_links(&scenario, path)))) {
        free(positions);
        return 1;
    }

    struct tsim_report report;
    clock_t started = clock();
    if (!tsim_scenario_run(&scenario, &report)) {
        fprintf(stderr, "%s: the run could not be set up\n", path);
        free(positions);
        free(links);
        return 1;
    }
    double cpu = (double)(clock() - started) / CLOCKS_PER_SEC;
    print_report(path, &scenario, &report);
    free(positions);
    free(links);
    fprintf(stderr, "%s: %" PRIu32 " nodes, %.1f simulated s in %.2f s of CPU\n", path,
            scenario.nodes, seconds(report.elapsed), cpu);
    return 0;
}
