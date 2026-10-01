#include "tsim/lora.h"

#include "check.h"

/* Expected values come from a separate implementation of the same Semtech formula, in Python, in
 * floating point: the airtime script behind the design record's tables (Research 04). The two
 * agree to the nanosecond, so a disagreement here is a bug in one of them. */
static const struct {
    uint8_t sf;
    uint32_t bw_hz;
    uint8_t cr;
    uint16_t preamble;
    uint32_t len;
    int64_t ns;
} cases[] = {
    {7, 125000, 1, 8, 16, 51456000},      /* 51.456 ms */
    {7, 125000, 1, 8, 64, 118016000},     /* 118.016 ms */
    {7, 125000, 1, 8, 200, 317696000},    /* 317.696 ms */
    {11, 125000, 1, 8, 16, 659456000},    /* 659.456 ms, LDRO on */
    {12, 125000, 1, 8, 200, 7217152000},  /* 7217.152 ms, LDRO on */
    {12, 500000, 1, 8, 64, 616448000},    /* 616.448 ms, LDRO off: 8.2 ms symbols */
    {12, 250000, 1, 8, 200, 3608576000},  /* 3608.576 ms, LDRO on */
    {10, 62500, 1, 8, 64, 1724416000},    /* 1724.416 ms, LDRO on */
    {11, 250000, 1, 16, 200, 1746944000}, /* 1746.944 ms, Meshtastic LONG_FAST shape */
    {11, 500000, 4, 16, 200, 1328128000}, /* 1328.128 ms, CR 4/8 */
    {7, 125000, 1, 8, 0, 25856000},       /* 25.856 ms, empty payload */
    {12, 125000, 4, 8, 255, 14032896000}, /* 14032.896 ms, the longest frame there is */
    {9, 250000, 2, 12, 16, 98816000},     /* 98.816 ms */
};

static void airtime_matches_reference(void) {
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct tsim_lora m = tsim_lora_default(cases[i].sf, cases[i].bw_hz);
        m.cr = cases[i].cr;
        m.preamble = cases[i].preamble;
        CHECK_EQ_I64(tsim_lora_airtime(&m, cases[i].len), cases[i].ns);
    }
}

static void symbol_is_two_to_the_sf_over_bw(void) {
    struct tsim_lora m = tsim_lora_default(7, 125000);
    CHECK_EQ_I64(tsim_lora_symbol(&m), 1024000);
    m = tsim_lora_default(12, 125000);
    CHECK_EQ_I64(tsim_lora_symbol(&m), 32768000);
    m = tsim_lora_default(7, 500000);
    CHECK_EQ_I64(tsim_lora_symbol(&m), 256000);
}

static void ldro_turns_on_at_sixteen_ms_symbols(void) {
    struct tsim_lora m = tsim_lora_default(10, 125000); /* 8.192 ms */
    CHECK(!tsim_lora_ldro(&m));
    m = tsim_lora_default(11, 125000); /* 16.384 ms */
    CHECK(tsim_lora_ldro(&m));
    m = tsim_lora_default(12, 500000); /* 8.192 ms */
    CHECK(!tsim_lora_ldro(&m));
    m.ldro = TSIM_LDRO_ON;
    CHECK(tsim_lora_ldro(&m));
    m = tsim_lora_default(12, 125000);
    m.ldro = TSIM_LDRO_OFF;
    CHECK(!tsim_lora_ldro(&m));
}

/* Twenty bits only save symbols when they take the frame under a block boundary: at SF7 a block
 * is 28 bits, so 10 bytes (96 bits explicit, 76 implicit) drops a block and 64 bytes does not. */
static void implicit_header_saves_twenty_bits(void) {
    struct tsim_lora m = tsim_lora_default(7, 125000);
    CHECK_EQ_I64(tsim_lora_payload_symbols(&m, 10), 8 + 4 * 5);
    CHECK_EQ_I64(tsim_lora_payload_symbols(&m, 64), 8 + 19 * 5);
    m.implicit_header = true;
    CHECK_EQ_I64(tsim_lora_payload_symbols(&m, 10), 8 + 3 * 5);
    CHECK_EQ_I64(tsim_lora_payload_symbols(&m, 64), 8 + 19 * 5);
}

static void payload_never_goes_below_eight_symbols(void) {
    struct tsim_lora m = tsim_lora_default(12, 125000);
    m.crc = false;
    m.implicit_header = true;
    CHECK_EQ_I64(tsim_lora_payload_symbols(&m, 0), 8);
}

static void airtime_grows_with_payload(void) {
    for (uint8_t sf = 7; sf <= 12; sf++) {
        struct tsim_lora m = tsim_lora_default(sf, 125000);
        tsim_time prev = tsim_lora_airtime(&m, 0);
        for (uint32_t len = 1; len <= 255; len++) {
            tsim_time t = tsim_lora_airtime(&m, len);
            CHECK(t >= prev);
            prev = t;
        }
    }
}

static void rejects_what_the_formula_does_not_cover(void) {
    struct tsim_lora m = tsim_lora_default(6, 125000);
    CHECK_EQ_I64(tsim_lora_airtime(&m, 16), -1);
    m = tsim_lora_default(13, 125000);
    CHECK_EQ_I64(tsim_lora_airtime(&m, 16), -1);
    m = tsim_lora_default(7, 0);
    CHECK_EQ_I64(tsim_lora_airtime(&m, 16), -1);
    m = tsim_lora_default(7, 125000);
    m.cr = 0;
    CHECK_EQ_I64(tsim_lora_airtime(&m, 16), -1);
    m.cr = 5;
    CHECK_EQ_I64(tsim_lora_airtime(&m, 16), -1);
    m = tsim_lora_default(7, 125000);
    CHECK_EQ_I64(tsim_lora_airtime(&m, 256), -1);
}

int main(void) {
    RUN(airtime_matches_reference);
    RUN(symbol_is_two_to_the_sf_over_bw);
    RUN(ldro_turns_on_at_sixteen_ms_symbols);
    RUN(implicit_header_saves_twenty_bits);
    RUN(payload_never_goes_below_eight_symbols);
    RUN(airtime_grows_with_payload);
    RUN(rejects_what_the_formula_does_not_cover);
    return CHECK_DONE();
}
