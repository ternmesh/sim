#ifndef TSIM_MESHCORE_H
#define TSIM_MESHCORE_H

#include <stdbool.h>
#include <stdint.h>

#include "tsim/lora.h"
#include "tsim/node.h"
#include "tsim/time.h"

/* Candidate 2: MeshCore's path routing, and the dispatcher it sends through.
 *
 * Both are ported from MeshCore (github.com/meshcore-dev/MeshCore, MIT licence) as tagged
 * repeater-v1.17.1 and companion-v1.17.1: src/Dispatcher.cpp, src/Mesh.cpp,
 * src/helpers/BaseChatMesh.cpp and the two examples' MyMesh.cpp. Where this port differs, the
 * difference is named below.
 *
 * A frame is MeshCore's: a header byte (route and payload type), two 2-byte region codes on a
 * scoped flood, a path length byte, the path - one hash of hash_size bytes per node it names - and
 * the payload. Every node has a hash, a pure function of its number; as with 1-byte public key
 * hashes, two nodes can share one.
 *
 * Messages.
 *
 *  - a broadcast is a channel message: a channel hash, a 2-byte MAC and the ciphertext, which is
 *    the sender's timestamp and a flags byte then the content, padded to 16-byte blocks. It floods,
 *    and every node that hears it holds it;
 *  - a direct message is the same behind a destination and a source hash. With no path to its
 *    destination it floods; with one, it goes direct along it. Its destination answers a flooded
 *    one with a path return - the path the flood took and the acknowledgement, encrypted, and
 *    flooded back - and a direct one with a bare acknowledgement, direct if it has a path back and
 *    flooded otherwise. A sender that gets a path return by flood stores the path and sends the
 *    path its return took back to the destination, direct, so both ends then have one;
 *  - a sender waits 500 ms plus 16 airtimes for a flood's acknowledgement, and for a direct one
 *    500 ms plus, for each hop and the last, 6 airtimes and 250 ms. Without one it tries again,
 *    up to `retries` times, each attempt a new packet; and an attempt after a direct one that went
 *    unanswered floods, the path forgotten. The firmware leaves retrying to the phone app, so this
 *    is the app's half, and MeshCore's own app may differ.
 *
 * Relaying. A node relays only if `relay` is set (a repeater; a companion does not).
 *
 *  - a flood is relayed once: a node that has not seen its packet hash - its type and payload, not
 *    its path - adds its own hash to the path and queues it 0 to 5t ms later, t being the airtime
 *    of what it will send, path and payload, times tx_delay_factor; nearer sources go first;
 *  - a direct packet is relayed by the node whose hash is first on the path, which takes its hash
 *    off and queues it 0 to 5t ms later, t this time by direct_tx_delay_factor, ahead of anything
 *    flooded. A direct acknowledgement goes on at once;
 *  - with rx_delay_base above 0, a received flood waits (rx_delay_base^(0.85 - score) - 1)
 *    airtimes before it is even looked at, the score growing with the SNR above the SF's floor and
 *    shrinking with the frame's length, so the best-heard copy is handled first. Under 50 ms it is
 *    not delayed at all, and never more than 32 s. MeshCore 1.17 ships it at 0: off.
 *
 * Each node remembers the last 160 packet hashes it has seen or sent, as the firmware does.
 *
 * cancel_heard is not MeshCore's: it is the first of MeshBench's eight protocol ideas, to drop a
 * flood relay on hearing another node relay the same packet first. MeshBench's study of it does
 * not publish its code, and says only that a node holding a flood drops it on hearing the packet
 * from somebody else; when it holds it is the question, so both are here. `waiting` drops a flood
 * heard again while it waits out its receive delay - it is still delivered, but never relayed - so
 * with no receive delay there is nothing to drop, as the study found. `queued` also drops a relay
 * heard again while it waits out its transmit delay, which MeshCore always has. estimate_cr is not
 * MeshCore's either: it reproduces MeshBench, whose firmware reckons every airtime at 4/5.
 *
 * Repeaters also send a zero-hop advert, an announce of 123 bytes, every advert_interval. The first
 * goes at a random point of the first interval rather than at boot, so the nodes of a run do not
 * all announce at once. The 47-hour flood advert, and adverts from companions, which the app sends
 * on demand, are not sent; every node knows every other's key from the start.
 *
 * A flood stops at 63 hops, the most its six-bit count holds, whatever flood_max says. MeshCore
 * lets a flood_max of 64 write a 64th hop into the hash size's bits, which garbles the frame.
 *
 * Not ported: regions - a scoped flood is relayed by every relay here, so a scenario lists as
 * relays only the repeaters that forward its scope - loop detection, which is off by default, extra
 * acknowledgements, which are off by default, and requests, responses and traces.
 *
 * The dispatcher, the MAC. A frame is due when its delay is up; the dispatcher sends the due frame
 * of the lowest priority number, first queued first among equals - except that here a frame joins
 * the node's queue only when it falls due, so equals go in the order they fell due. Before sending
 * it looks whether the radio is part-way through receiving a frame; if so it waits 120, 240 or
 * 360 ms and looks again, until the channel has been busy for 4 s, when it sends anyway. It keeps
 * a duty cycle budget of 1/(1 + airtime_factor) over an hour, starting full, and waits when the
 * budget will not cover half a maximum-length frame. MeshCore polls in a loop; this port acts at
 * once.
 *
 * latched_header is not MeshCore's: it reproduces MeshBench v0.1.0, whose radio does not clear its
 * header-valid flag when the driver reads a packet. MeshCore's CustomSX1262::isReceiving() takes
 * that flag to mean the channel is busy, until a stale-flag timeout of 3934 ms clears it;
 * RadioLib's readData() clears it at once on a real radio. Set to that timeout, a repeater finds
 * the channel busy for 3.9 s from the first header it hears, then relays whatever it is hearing,
 * and the repeaters that heard the same frame relay together. */

/* The node's hash, its first `size` bytes in `out`, size 1 to 3. */
void tsim_meshcore_hash(uint32_t node, uint8_t size, uint8_t *out);

struct tsim_meshcore_mac_config {
    double airtime_factor; /* the budget is a share 1/(1 + this) of the hour; at least 0 */
    /* 0, or how long the radio's header flag stays set once a header sets it: MeshBench's. */
    tsim_time latched_header;
};

/* MeshCore's default: an airtime factor of 1, half the hour. */
struct tsim_meshcore_mac_config tsim_meshcore_mac_default(void);

extern const struct tsim_mac tsim_meshcore_mac;

#define TSIM_MESHCORE_PATH_MAX 64 /* bytes of path a frame can carry */

/* When a flood heard again is dropped: see cancel_heard above. */
enum tsim_meshcore_cancel {
    TSIM_MESHCORE_CANCEL_NO,
    TSIM_MESHCORE_CANCEL_WAITING, /* while it waits out its receive delay */
    TSIM_MESHCORE_CANCEL_QUEUED,  /* then too while its relay waits to be sent */
};
#define TSIM_MESHCORE_PAYLOAD_MAX 184

struct tsim_meshcore_config {
    uint16_t channel;
    struct tsim_lora lora;
    double tx_dbm;
    /* Which nodes relay, as a list of numbers and ranges - "0-45,50" - or "all". Every other node
     * is a companion. */
    char relays[96];
    uint8_t hash_size;      /* bytes per path entry, 1 to 3 */
    bool scoped;            /* floods carry region codes, as a companion with a scope sends */
    uint8_t flood_max;      /* a relay does not relay a flood that has this many hops, 1..64 */
    double rx_delay_base;   /* 0 for none, or above 1 and at most 20 */
    double tx_delay_factor; /* 0 to 2 */
    double direct_tx_delay_factor;          /* 0 to 2 */
    uint8_t retries;                        /* attempts after the first */
    tsim_time advert_interval;              /* a repeater's zero-hop advert, or 0 for none */
    enum tsim_meshcore_cancel cancel_heard; /* MeshBench's idea 1; not MeshCore's */
    /* The coding rate the firmware's own airtime estimate assumes, which every delay and timeout
     * is reckoned from: 0 for the radio's, as on real hardware; 1 to 4 to reproduce MeshBench,
     * whose firmware estimates at 4/5 whatever the air runs at. */
    uint8_t estimate_cr;
};

/* MeshCore 1.17's defaults: every node a relay, 1-byte hashes, scoped floods, no receive delay,
 * tx_delay_factor 0.5, direct_tx_delay_factor 0.3, a flood limit of 64, three retries and a
 * two-minute advert. */
struct tsim_meshcore_config tsim_meshcore_default(uint16_t channel, const struct tsim_lora *lora,
                                                  double tx_dbm);

/* Whether `spec` is a list tsim_meshcore_config.relays can hold. */
bool tsim_meshcore_relays_valid(const char *spec);

extern const struct tsim_routing tsim_meshcore;

#endif
