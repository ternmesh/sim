#ifndef TSIM_TOOLS_JSON_H
#define TSIM_TOOLS_JSON_H

#include <stdio.h>

/* Writes `s` as a JSON string, quotes included. A string from outside - a path can hold any byte
 * but '\0' - comes out as valid JSON and valid UTF-8: quotes, backslashes and control bytes are
 * escaped, well-formed UTF-8 passes through, and each byte of anything else becomes U+FFFD. An
 * escape such as ÿ would claim a character the path never held. */
void json_string(FILE *out, const char *s);

#endif
