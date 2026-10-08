#ifndef TSIM_LISTEN_H
#define TSIM_LISTEN_H

#include <stdint.h>

#include "tsim/lora.h"
#include "tsim/node.h"
#include "tsim/time.h"

/* Listening before sending, as a radio can.
 *
 * A radio knows the channel is in use only when it is receiving a frame, and only once it has been
 * on that frame for `detect`: the time its demodulator takes to find a preamble, and its driver to
 * hear of it. A frame it decides to send is on the air `turnaround` later, and in that time it
 * hears nothing. So a frame that began less than detect + turnaround before this node's goes
 * unnoticed, and the two meet; and so does a frame the radio is not on at all - one too weak to
 * receive, one with another sync word past its preamble, or one whose start it missed.
 *
 * When there is a frame to send, the MAC looks. If the radio has been on a frame for `detect` it
 * waits for that frame's end, then 0 to `window` slots more, drawn evenly, and looks again.
 * Otherwise it sends, `turnaround` later, without looking again.
 *
 * tsim_meshcore_mac listens too, but notices a frame the instant it starts, which no radio does,
 * and tsim_aloha does not listen at all. */
struct tsim_listen_config {
    tsim_time detect;
    tsim_time turnaround;
    tsim_time slot;
    uint32_t window;
};

/* For a modulation: five symbols and a millisecond to notice a frame - the radio model's default
 * lock, and a driver that asks its chip every millisecond - a millisecond to turn round, a slot
 * of the two together, and no window: it sends as the frame it waited for ends. */
struct tsim_listen_config tsim_listen_default(const struct tsim_lora *lora);

extern const struct tsim_mac tsim_listen;

#endif
