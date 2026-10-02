#ifndef TSIM_NODESET_H
#define TSIM_NODESET_H

#include <stdint.h>

/* A set of nodes written as text, as a scenario names a candidate's relays: "all", or node numbers
 * and ranges separated by commas, such as "0-45,50". */

/* 1 if `node` is in `spec`, 0 if not, or -1 if the spec does not parse. */
int tsim_nodeset_contains(const char *spec, uint32_t node);

#endif
