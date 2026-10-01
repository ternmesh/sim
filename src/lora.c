#include "tsim/lora.h"

/* All of it is integer arithmetic. A symbol is 2^SF / BW seconds, so every duration below is
 * (a count of quarter-symbols) * 2^SF * 1e9 / (4 * BW) nanoseconds, computed once and rounded
 * once. The quarter-symbols are there for the preamble's extra 4.25 symbols. */

#define NS_PER_S 1000000000ULL

struct tsim_lora tsim_lora_default(uint8_t sf, uint32_t bw_hz) {
    return (struct tsim_lora){
        .sf = sf,
        .bw_hz = bw_hz,
        .cr = 1,
        .preamble = 8,
        .implicit_header = false,
        .crc = true,
        .ldro = TSIM_LDRO_AUTO,
    };
}

bool tsim_lora_valid(const struct tsim_lora *m) {
    return m->sf >= 7 && m->sf <= 12 && m->bw_hz > 0 && m->cr >= 1 && m->cr <= 4 &&
           (m->ldro == TSIM_LDRO_AUTO || m->ldro == TSIM_LDRO_OFF || m->ldro == TSIM_LDRO_ON);
}

bool tsim_lora_ldro(const struct tsim_lora *m) {
    switch (m->ldro) {
    case TSIM_LDRO_ON:
        return true;
    case TSIM_LDRO_OFF:
        return false;
    case TSIM_LDRO_AUTO:
    default:
        /* 2^SF / BW >= 16 ms, without the division. */
        return ((uint64_t)1 << m->sf) * 1000 >= (uint64_t)m->bw_hz * 16;
    }
}

static tsim_time quarter_symbols_to_ns(const struct tsim_lora *m, uint64_t quarters) {
    uint64_t num = quarters * ((uint64_t)1 << m->sf) * NS_PER_S;
    uint64_t den = (uint64_t)m->bw_hz * 4;
    return (tsim_time)((num + den / 2) / den);
}

tsim_time tsim_lora_symbol(const struct tsim_lora *m) { return quarter_symbols_to_ns(m, 4); }

uint32_t tsim_lora_payload_symbols(const struct tsim_lora *m, uint32_t payload_len) {
    int64_t bits = 8 * (int64_t)payload_len - 4 * (int64_t)m->sf + 28 + (m->crc ? 16 : 0) -
                   (m->implicit_header ? 20 : 0);
    int64_t per_block = 4 * ((int64_t)m->sf - (tsim_lora_ldro(m) ? 2 : 0));
    int64_t blocks = bits > 0 ? (bits + per_block - 1) / per_block : 0;
    return (uint32_t)(8 + blocks * (m->cr + 4));
}

tsim_time tsim_lora_airtime(const struct tsim_lora *m, uint32_t payload_len) {
    if (!tsim_lora_valid(m) || payload_len > 255) {
        return -1;
    }
    /* (preamble + 4.25) symbols, then the payload symbols, all in quarters. */
    uint64_t quarters =
        4 * (uint64_t)m->preamble + 17 + 4 * (uint64_t)tsim_lora_payload_symbols(m, payload_len);
    return quarter_symbols_to_ns(m, quarters);
}
