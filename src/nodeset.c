#include "tsim/nodeset.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int tsim_nodeset_contains(const char *spec, uint32_t node) {
    if (strcmp(spec, "all") == 0) {
        return 1;
    }
    int found = 0;
    const char *p = spec;
    while (*p) {
        char *end;
        if (!isdigit((unsigned char)*p)) {
            return -1;
        }
        unsigned long lo = strtoul(p, &end, 10), hi = lo;
        p = end;
        if (*p == '-') {
            p++;
            if (!isdigit((unsigned char)*p)) {
                return -1;
            }
            hi = strtoul(p, &end, 10);
            p = end;
            if (hi < lo) {
                return -1;
            }
        }
        if (node >= lo && node <= hi) {
            found = 1;
        }
        if (*p == ',') {
            p++;
            if (!*p) {
                return -1;
            }
        } else if (*p) {
            return -1;
        }
    }
    return p == spec ? -1 : found;
}
