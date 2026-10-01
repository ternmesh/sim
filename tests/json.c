#include "json.h"

#include <stdbool.h>
#include <string.h>

#include "check.h"

/* U+FFFD as JSON writes it: what a byte that is not UTF-8 becomes. */
#define REPL "\\ufffd"

/* Whether json_string() writes `want` for `in`. */
static bool writes(const char *in, const char *want) {
    char got[256] = {0};
    FILE *f = tmpfile();
    if (!f) {
        return false;
    }
    json_string(f, in);
    rewind(f);
    size_t len = fread(got, 1, sizeof got - 1, f);
    fclose(f);
    got[len] = '\0';
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "  wrote %s, expected %s\n", got, want);
        return false;
    }
    return true;
}

static void plain_text_is_quoted(void) {
    CHECK(writes("scenarios/town.tsim", "\"scenarios/town.tsim\""));
    CHECK(writes("", "\"\""));
}

static void quotes_backslashes_and_control_bytes_are_escaped(void) {
    CHECK(writes("a\"b\\c", "\"a\\\"b\\\\c\""));
    CHECK(writes("tab\tnew\nline\x01\x1f", "\"tab\\u0009new\\u000aline\\u0001\\u001f\""));
    CHECK(writes("\x7f", "\"\x7f\"")); /* DEL is not a control byte to JSON */
}

static void well_formed_utf8_passes_through(void) {
    CHECK(writes("caf\xc3\xa9", "\"caf\xc3\xa9\""));           /* U+00E9 */
    CHECK(writes("\xe2\x82\xac", "\"\xe2\x82\xac\""));         /* U+20AC */
    CHECK(writes("\xf0\x9f\x98\x80", "\"\xf0\x9f\x98\x80\"")); /* U+1F600 */
    CHECK(writes("\xf4\x8f\xbf\xbf", "\"\xf4\x8f\xbf\xbf\"")); /* U+10FFFF, the last */
    CHECK(writes("\xef\xbf\xbd", "\"\xef\xbf\xbd\""));         /* U+FFFD itself */
}

/* Each byte of anything that is not UTF-8 becomes U+FFFD, and what follows is read afresh. */
static void every_other_byte_becomes_a_replacement(void) {
    CHECK(writes("a\xff"
                 "b",
                 "\"a" REPL "b\""));
    CHECK(writes("\x80", "\"" REPL "\""));                            /* a stray continuation */
    CHECK(writes("\xc3", "\"" REPL "\""));                            /* cut short by the end */
    CHECK(writes("\xe2\x82x", "\"" REPL REPL "x\""));                 /* cut short by a letter */
    CHECK(writes("\xc0\xaf", "\"" REPL REPL "\""));                   /* an overlong '/' */
    CHECK(writes("\xed\xa0\x80", "\"" REPL REPL REPL "\""));          /* a surrogate */
    CHECK(writes("\xf4\x90\x80\x80", "\"" REPL REPL REPL REPL "\"")); /* past U+10FFFF */
    CHECK(writes("\xc3\xa9\xff\xc3\xa9", "\"\xc3\xa9" REPL "\xc3\xa9\""));
}

int main(void) {
    RUN(plain_text_is_quoted);
    RUN(quotes_backslashes_and_control_bytes_are_escaped);
    RUN(well_formed_utf8_passes_through);
    RUN(every_other_byte_becomes_a_replacement);
    return CHECK_DONE();
}
