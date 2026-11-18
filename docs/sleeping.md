# Leaves that sleep, measured

The specification's `draft/sleeping.md` (ternmesh/spec#62) lets a leaf turn its radio off between
wakes. The leaf polls a **keeper**, a relay next to it, at least every `w` (its wake). The keeper
holds the leaf's frames while it sleeps, hands them over when it polls, and sends the node before
it a held notice so that hop does not fail. The draft was written before anything was simulated.
This page reports what candidate 3 does with it on the region, and which of the draft's numbers
and rules that changes. `routing.sleep` turns it on; see [scenarios.md](scenarios.md) for the
keys.

## The answer

- **As drafted, it does not work on a busy mesh.** On the region at 10 dBm (as
  `scale/region-distvec-fast.tsim`, where every leaf listening gets 98.1% of unicasts on time),
  half the leaves sleep with a 300 s wake. Only 64.6% of messages to them ever arrived. Messages
  to the leaves that listen fell to 69.5% within 60 s. Four things in the draft cause it, each
  shown by turning it off alone (seed 1, below):
  1. **Keepers pay for poll answers from the requests' share of the cap** (0.125% of their
     time). Relays spend that share on seqno requests, so most answers were never sent: 10% of
     polls answered. Each unanswered poll is retried after 10 s, which drains the cap further.
     Charged as an acknowledgement is, never held, keepers answered every poll they heard.
  2. **`POLL_TRIES` = 3 is too few.** About 40% of poll exchanges are lost on this channel
     (keepers busy sending, collisions). Three misses in a row happen often enough that leaves
     changed keeper 2 times an hour each, and every change raises the leaf's seq across the mesh.
     At 6 it fell to 0.12 an hour, and delivery to sleepers went from 74% to 91%.
  3. **Staying awake for one's own acknowledgement costs most of the energy.** A sleeper whose
     message goes to another sleeper waits `w` longer for the acknowledgement, awake. Letting it
     sleep, with its keeper holding the acknowledgement and the source adding its own `w` to the
     wait, cut radio-on time from 9.3% to 1.3% with delivery the same.
  4. **Routes to a sleeping leaf cannot be repaired by asking it.** A relay whose route to a leaf
     starves sends a seqno request, which only the leaf can answer, and it is asleep. These were
     nearly all the remaining losses (no route, at relays part-way). Announcing every 10 minutes
     instead of every hour, with the seq raised each time, took delivery to sleepers from 90% to
     99.7%.
- **With those four changes** (`scenarios/sleep/region-tuned.tsim`), 98.4% of messages to
  sleepers arrive, 82% within one wake plus 60 s, at a median of 197 s. Their radios are on
  1.5% of the time against 100% for a leaf that listens: about 150 µA against 4,650 µA, radio
  only, estimated (below). Leaves that listen still get 91.4% of unicasts within 60 s, against
  98.1% with no sleepers. Most of that difference is polls: they take 6.6% of the mesh's airtime.
- **Short wakes cost the whole mesh.** At `w` of 30, 60 and 120 s, polls take 13 to 17% of all
  airtime, fewer than half are answered, and delivery to sleepers falls to 57-84%, with
  listening leaves down to 69-78% within 60 s. From 300 s up everything delivers 98-99.7%.
  `WAKE_MIN` should be 300 s, not 30 s, unless polls get a share of their own.
- **`HOLD_LEAF` = 8 is too few at an hour's wake.** At `w` = 3600 s keepers reached 8 frames
  for one leaf and dropped 47 frames in three hours. At 16 the peak was 12 and nothing was
  dropped. At 300 s the peak was 5.
- **`LISTEN_MIN` = 2 s is enough at SF7.** At 1 s delivery to sleepers dropped 1.3 points; 4 s
  gained nothing and cost 50% more radio-on time. At slower presets it needs measuring again:
  an answer and the first held frame take longer to arrive.
- **The draft's energy estimates are low by 3-5×.** It says about 50 µA polling every 5 minutes
  and about 10 µA hourly. Measured here: 150 µA and 51 µA. The difference comes from the leaf's
  own traffic (two messages an hour and their retries), polls lost and retried, and announces.
  Against a leaf that listens, sleeping still saves 31× at 300 s and 91× at an hour.

## Results

Region, 1000 nodes over 18 × 18 km at SF7/125 kHz and 10 dBm, 200 relays (cds) and 800 leaves,
half of them sleeping. Each node sends a message every 30 minutes, a quarter of them broadcasts.
Six hours of warmup, an hour measured, and three hours more for held messages to arrive.
Three seeds each. "In wake" means within `w` + 60 s. Current is the radio's alone, estimated
from SX1262 datasheet figures (4.6 mA receiving, 118 mA sending at 22 dBm, 1 µA asleep). It is
not a measurement, and the board's processor is left out.

| Run | To sleepers: delivered / in 60 s / in wake | Median, p95 latency | To listening leaves in 60 s | Sleeper radio on | Est. µA (listener ~4,650) | Polls/h, answered | Polls' share of airtime | Peak held, dropped full |
|---|---|---|---|---|---|---|---|---|
| As drafted, w 300 s | 64.6 / 13.4 / 34.2% | 354 s, 1070 s | 69.5% | 26.9% | 1,492 | 57, 10% | 8.3% | 4, 0 |
| Tuned, w 30 s | 56.5 / 20.7 / 29.5% | 85 s, 241 s | 72.1% | 5.5% | 476 | 82, 42% | 16.1% | 4, 0 |
| Tuned, w 60 s | 67.3 / 21.4 / 38.8% | 98 s, 348 s | 69.2% | 5.1% | 452 | 82, 45% | 16.5% | 4, 0 |
| Tuned, w 120 s | 83.5 / 20.0 / 59.3% | 111 s, 482 s | 77.9% | 3.8% | 341 | 58, 46% | 13.1% | 4, 0 |
| **Tuned, w 300 s** | **98.4 / 12.7 / 82.4%** | 197 s, 762 s | 91.4% | 1.5% | 150 | 20, 57% | 6.6% | 5, 0 |
| Tuned, w 900 s | 99.7 / 5.1 / 86.4% | 519 s, 1873 s | 94.8% | 0.9% | 87 | 6.5, 61% | 2.4% | 6, 0 |
| Tuned, w 3600 s | 99.7 / 1.4 / 88.5% | 2033 s, 6857 s | 95.8% | 0.5% | 51 | 1.6, 63% | 0.6% | 8, 47 |
| Tuned, w 3600 s, hold 16 | 99.7 / 1.4 / 88.3% | 2053 s, 6868 s | 95.7% | 0.5% | 51 | 1.6, 65% | 0.6% | 12, 0 |
| Tuned, w 300 s, listen 1 s | 97.1 / 13.3 / 76.3% | 208 s, 789 s | 90.5% | 1.4% | 149 | 22, 54% | 6.6% | 6, 0 |
| Tuned, w 300 s, listen 4 s | 98.2 / 15.8 / 83.4% | 188 s, 781 s | 91.3% | 2.3% | 186 | 21, 56% | 6.7% | 5, 0 |
| Tuned, w 300 s, every leaf | 91.0 / 8.9 / 57.5% | 257 s, 1610 s | — | 2.9% | 242 | 26, 43% | 11.3% | 6, 0 |
| Tuned, w 300 s, 0 dBm | 92.9 / 14.3 / 81.1% | 181 s, 673 s | 83.7% | 4.1% | 251 | 14, 81% | 4.7% | 5, 0 |

With every leaf listening, unicast on time is 98.1% at 10 dBm and 91.2% at 0 dBm. At 0 dBm,
sleepers are reached about as well as anything else in the sparser mesh.

The four changes, one at a time (seed 1, w 300 s, delivered to sleepers / radio on):

| | To sleepers delivered | Polls answered | Keepers per sleeper-hour | Sleeper radio on |
|---|---|---|---|---|
| As drafted | 68.5% | 10% | 15 | 26% |
| Answers not capped | 73.8% | 49% | 2.0 | 14.5% |
| and `POLL_TRIES` 6 | 91.1% | 57% | 0.12 | 9.3% |
| and asleep while its acknowledgement waits | 89.9% | 58% | 0.12 | 1.3% |
| and an announce every 10 min with a new seq | 99.7% | 59% | 0.12 | 1.2% |

## What was built

In candidate 3 (`src/distvec.c`, *Leaves that sleep*), as close to the draft as the simulator's
frames allow:

- A leaf picked to sleep (`routing.sleep` of the leaves, by node number) starts half an hour
  after it does. It takes its anchor as keeper, raises its seq, and from then on announces only
  with its polls, setting a *sleeps* flag. It sends everything to its keeper. Its radio is off
  (`tsim_node_radio()`, new in `tsim/node.h`) except for `listen` after each frame it sends, and
  while a frame is queued, being sent or received, or a hop of its own waits. With
  `routing.ack_awake` it also stays on while its own message waits for an acknowledgement.
- Polls are 15 bytes and answers 14, as in the draft. A leaf whose poll goes unanswered polls
  again after `poll_retry`. After `poll_tries` misses in a row it takes its keeper out of use and
  takes its next anchor as keeper, raising its seq. With no anchor left it stops sleeping and
  solicits.
- A relay learns it is a leaf's keeper from the leaf's polls. Any other relay that hears the
  *sleeps* flag no longer uses the leaf as a next hop, so routes go through the keeper. The keeper
  holds data frames and acknowledgements for the leaf while it is not awake, up to `hold_leaf`
  each, for at most `2w + 60 s`. It sends the node before a 15-byte held notice, and after
  answering a poll hands frames over, acknowledgements first. A frame lost on the way to the leaf
  while it is awake is held once more, without counting against the link. When a leaf names
  another keeper, or stops sleeping, the old keeper passes on what it held.
- A source sending to a sleeper waits `w` + `listen` longer per try.

**Simplified:**
- The draft's end-to-end sleep notice (node frame kind `0x04`) is a table shared by every node,
  standing for what each session would hold. Its frames are not sent.
- The announce's `keeper` field (4 bytes) is not sent. The keeper is learned from polls, so
  sleepers' announces are 4 bytes short of the draft.
- The held notice carries the simulator's frame identity, not the draft's tag. It is the same
  length.
- First-contact frames are not modelled, so none are held.
- The firmware's routing (`core`) is not changed. That needs the firmware to implement the draft,
  and should be measured again once it does.

The report gains a `sleep` section with all of the above. Its numbers cover the window only, and
messages are followed until the run ends.

## Recommended for the draft

1. Pay for poll answers like acknowledgements (never held), or from a share of their own, not
   from the requests' share of the cap.
2. `POLL_TRIES` from 3 to 6.
3. A sleeping leaf need not listen while its own message waits for its acknowledgement. Its
   keeper holds the acknowledgement, and the leaf adds its own `w` to its wait. This replaces the
   draft's "SHOULD poll again before `l` runs out."
4. `SLEEP_ANNOUNCE` from 3600 s to 600 s, and a sleeping leaf raises its seq with each announce,
   since a seqno request cannot reach it while it sleeps. Alternatively, a keeper could answer
   requests for its leaf.
5. `WAKE_MIN` from 30 s to 300 s, or give polls their own capped share of airtime.
6. `HOLD_LEAF` from 8 to 16.
7. The Rationale's current estimates (about 50 µA at 5 minutes, about 10 µA hourly) to about
   150 µA and 50 µA of radio current at the comparison's traffic, still to be measured on a board.

## Not measured

- Other presets: the deployed preset's slower frames, and with them `LISTEN_MIN`.
- Sleepers that move.
- The firmware `core` routing, whose draft this is.
- Churn of relays, and so of keepers.
