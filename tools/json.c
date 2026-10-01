#include "json.h"

#include <stdint.h>

/* The length of the well-formed UTF-8 sequence at `c`, or 0 if there is none: a stray
 * continuation byte, a sequence cut short (by the terminator too), an overlong encoding, a
 * surrogate, or a code point past U+10FFFF. */
static int utf8_length(const unsigned char *c) {
    int n;
    uint32_t cp, min;
    if (c[0] < 0x80) {
        return 1;
    } else if ((c[0] & 0xE0) == 0xC0) {
        n = 2;
        cp = c[0] & 0x1Fu;
        min = 0x80;
    } else if ((c[0] & 0xF0) == 0xE0) {
        n = 3;
        cp = c[0] & 0x0Fu;
        min = 0x800;
    } else if ((c[0] & 0xF8) == 0xF0) {
        n = 4;
        cp = c[0] & 0x07u;
        min = 0x10000;
    } else {
        return 0;
    }
    for (int i = 1; i < n; i++) {
        if ((c[i] & 0xC0) != 0x80) {
            return 0;
        }
        cp = cp << 6 | (c[i] & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    return n;
}

void json_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *c = (const unsigned char *)s; *c;) {
        int n = utf8_length(c);
        if (n == 0) {
            fputs("\\ufffd", out);
            c++;
        } else if (*c == '"' || *c == '\\') {
            fputc('\\', out);
            fputc(*c++, out);
        } else if (*c < 0x20) {
            fprintf(out, "\\u%04x", *c++);
        } else {
            fwrite(c, 1, (size_t)n, out);
            c += n;
        }
    }
    fputc('"', out);
}
