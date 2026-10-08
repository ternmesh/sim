# Sync-word cross-reception

Whether a LoRa radio set to one sync word receives frames sent with another. Tern's sync word
(`0x5E`, alternate `0x67`: the specification's `draft/phy.md`) was chosen so that Meshtastic
(`0x2B`), MeshCore (`0x12`) and LoRaWAN (`0x34`) radios do not receive Tern's frames, nor Tern's
theirs. This is that claim, observed.

## What was run: Tern's driver with each word

Two Heltec V3 boards (SX1262), each with the antenna sold with it, 0.3 m apart indoors, on
2026-10-07. Both ran Tern's demo (ternmesh/firmware at `107c685`, `ports/heltec-v3`), whose bench
commands set the sync word and count what the receiver saw. US915 profile: 921.25 MHz, 500 kHz,
SF9, CR 4/5, 16-symbol preamble, explicit header, CRC on.

For every pair of the five sync words, one board sent 100 frames of 24 bytes, 150 ms apart, and
the other listened: at -9 and +22 dBm, in both directions. That is 100 runs and 10,000 frames.
Before them each board listened with each sync word for 15 s with nothing sent.

    run.py /dev/cu.usbserial-0001 /dev/cu.usbserial-4 > us915-sf9-bw500.csv

The radio takes a two-byte sync word, `0xXY` becoming `0xX4Y4`, which is what RadioLib writes
for Meshtastic and MeshCore: `0x24B4` and `0x1424`.

### What was found

| Sent with | Heard with | Frames sent | Preambles | Headers | Header errors | Frames |
|---|---|---|---|---|---|---|
| the same word (5 words) | | 2000 | 2001 | 2000 | 0 | 2000 |
| `0x5E` | any other | 1600 | 1608 | 0 | 0 | 0 |
| any other | `0x5E` | 1600 | 1607 | 0 | 0 | 0 |
| `0x67` | any other | 1600 | 1610 | 0 | 0 | 0 |
| any other | `0x67` | 1600 | 1606 | 0 | 0 | 0 |
| `0x2B` | `0x12` | 400 | 401 | 0 | 64 | 0 |
| `0x12` | `0x2B` | 400 | 410 | 0 | 73 | 0 |
| every other pair that differs | | 7200 | 7232 | 0 | 0 | 0 |

(The rows overlap: a pair of `0x5E` and `0x67` is in four of them.)

* **A different sync word was never received.** 8000 frames sent with one word and listened for
  with another gave no frame and no header that checked.
* **`0x5E` and `0x67` leaked nothing either way**, against each other or the other three: the
  receiver saw each frame's preamble and went back to listening.
* **Meshtastic's and MeshCore's words are not as far apart.** With `0x2B` sent and `0x12`
  listened for, or the reverse, the receiver took 9 to 32 frames in every 100 as far as a header,
  which then failed its check. It happened in all eight such runs, both directions and both
  powers, and in no other pair. No frame came of it, but a receiver that far in is not listening
  for anything else until the header fails. This is between two other networks, not Tern and
  either, and it is at one modulation.
* With matching words every frame arrived: 2000 of 2000, at about -29 dBm for -9 dBm sent and
  0 dBm for +22 dBm.
* With nothing sent, board A counted three preambles in 75 s and board B none in 75 s. The few
  preambles over 100 in a run are of that kind.

## Against the other projects' own firmware

The same two boards on 2026-10-07, one running Tern's demo (ternmesh/firmware at `5178f22`) and the
other, in turn, Meshtastic 2.7.26 and MeshCore 1.17.1 (a companion build). Each was moved to a
frequency of its own with a private channel and no relaying, and the Tern board put on it with
its modulation. The other firmware's sync word cannot be changed, so each run is that word
against one of Tern's board's, in both directions. `vs_meshtastic.py` and `vs_meshcore.py` ran
them; what the other firmware sent and received is from its own log or counters.

**Meshtastic**: 926.875 MHz, SF11, 250 kHz, sync word `0x2B`. `meshtastic-*.csv`.

| Tern's word | Meshtastic's frames received by Tern | Tern's frames received by Meshtastic |
|---|---|---|
| `0x2B`, the control | 121 of 121 | 119 of 120 |
| `0x5E` | 0 of 160 | 0 of 160 |
| `0x67` | 0 of 161 | 0 of 160 |
| `0x12` | 0 of 60, and 13 header errors | 0 of 60 |
| `0x34` | 0 of 60 | 0 of 60 |

**MeshCore**: 926.5 MHz, SF7, 62.5 kHz, sync word `0x12`. `meshcore-*.csv`.

| Tern's word | MeshCore's frames received by Tern | Tern's frames received by MeshCore, at +2 dBm | at +22 dBm |
|---|---|---|---|
| `0x12`, the control | 120 of 120 | 60 of 60 | 32 of 60 |
| `0x5E` | 0 of 260, and 7 header errors | 0 of 230 | 0 of 30 |
| `0x67` | 0 of 260, and 13 header errors | 0 of 230 | 0 of 30 |
| `0x2B` | 0 of 260, and 14 header errors | 2 of 230 | 0 of 30 |
| `0x34` | 0 of 60, and 5 header errors | 0 of 30 | 0 of 30 |

* **Both projects write the sync words assumed above.** Set to theirs, the Tern board received
  every frame each sent, and each received Tern's.
* **Neither received a frame sent with `0x5E` or `0x67`, and Tern with either received none of
  theirs.** 420 frames each way for each word.
* **The header errors against MeshCore are not about the word.** MeshCore's frames here were
  111 bytes at SF7 and 62.5 kHz, and a receiver with any other word, `0x5E` among them, saw
  about three preambles in each and now and then took what followed for a header, which failed:
  3 to 5 in every 100 frames, much the same for every word. That is a receiver finding a false
  start inside a long frame, which costs it the time until the header fails. Nothing was
  received. It did not happen with the 24-byte test frames or with Meshtastic's at SF11.
* **MeshCore received two frames sent with Meshtastic's word**, one in 30 and one in 200, with
  none received in seven minutes of nothing sent. With the header errors between these two words
  in every other run, that is most likely a real crossing: now and then a MeshCore radio
  receives a whole Meshtastic frame, were the two on one channel and modulation.
* **At +22 dBm MeshCore received half the control's frames**, and counted no errors. The boards
  are 0.3 m apart, about 0 dBm at the receiver, so that column says less than the others.
* **Other people's frames arrive.** In five minutes with nothing sent, the Meshtastic board
  received a frame from a node at -114 dBm. In the first run its counters moved by one frame
  received and two bad during Tern's `0x67` and `0x5E` runs at +22 dBm; the run was repeated
  with 100 frames of each and the log kept (`*-recheck.csv`), and nothing of Tern's was received
  or counted bad.

## What this does not show

* **Weak signals.** Every run was at 0.3 m: -30 to 0 dBm received. A filter that leaks only near
  the noise would not show here. That needs attenuators or distance.
* **Other radios.** The SX1276 and LR1110 were not tested.
* **Other versions and builds** of Meshtastic and MeshCore than the two named, and repeaters.
* One pair of boards, one room, one afternoon.
