#ifndef TSIM_LORA_H
#define TSIM_LORA_H

#include <stdbool.h>
#include <stdint.h>

#include "tsim/time.h"

/* LoRa modulation and time on air.
 *
 * The time-on-air formula is Semtech's, as published for the SX126x and SX127x (application note
 * AN1200.13 and the SX1261/2 datasheet), and is the same arithmetic as
 * Research 04's airtime script, whose tables the tests check against. Only SF7 to SF12 are
 * accepted: SF5 and SF6 use a different preamble and payload rule on the SX126x, and nothing in
 * the design uses them yet. */

enum tsim_ldro {
    TSIM_LDRO_AUTO = 0, /* on when a symbol lasts 16 ms or longer, as the radio drivers do */
    TSIM_LDRO_OFF,
    TSIM_LDRO_ON,
};

struct tsim_lora {
    uint8_t sf;        /* spreading factor, 7..12 */
    uint32_t bw_hz;    /* bandwidth: 62500, 125000, 250000 or 500000 (any value > 0 computes) */
    uint8_t cr;        /* coding rate 4/(4+cr), cr = 1..4 */
    uint16_t preamble; /* programmed preamble length in symbols */
    bool implicit_header;
    bool crc;
    enum tsim_ldro ldro;
};

/* SF and BW with the defaults everything else in the design assumes: CR 4/5, an 8-symbol
 * preamble, explicit header, CRC on, LDRO automatic. */
struct tsim_lora tsim_lora_default(uint8_t sf, uint32_t bw_hz);

/* Whether the parameters are ones the time-on-air formula covers. */
bool tsim_lora_valid(const struct tsim_lora *m);

/* Whether low data rate optimisation is in effect for these parameters. */
bool tsim_lora_ldro(const struct tsim_lora *m);

/* The duration of one symbol, 2^SF / BW, rounded to the nearest nanosecond. */
tsim_time tsim_lora_symbol(const struct tsim_lora *m);

/* The number of symbols after the preamble: header, payload and CRC. */
uint32_t tsim_lora_payload_symbols(const struct tsim_lora *m, uint32_t payload_len);

/* Time on air for a frame carrying payload_len bytes, rounded to the nearest nanosecond.
 * Returns -1 for parameters tsim_lora_valid() rejects or a payload over 255 bytes. */
tsim_time tsim_lora_airtime(const struct tsim_lora *m, uint32_t payload_len);

#endif
