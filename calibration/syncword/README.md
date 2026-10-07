# Sync-word cross-reception

Whether a LoRa radio set to one sync word receives frames sent with another. Tern's sync word
(`0x5E`, alternate `0x67`: the specification's `draft/phy.md`) was chosen so that Meshtastic
(`0x2B`), MeshCore (`0x12`) and LoRaWAN (`0x34`) radios do not receive Tern's frames, nor Tern's
theirs. This is that claim, observed.

## What was run

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

## What was found

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

## What this does not show

* **Weak signals.** Both powers are strong at 0.3 m (-29 and 0 dBm received). A filter that
  leaks only near the noise would not show here. That needs attenuators or distance.
* **Other firmware.** Both boards ran Tern's driver with the other networks' sync words. That the
  sync-word match is the chip's makes this the same test, provided those projects write the
  register values above; a run with their firmware on one board would confirm that.
* **Other modulations.** Only SF9 at 500 kHz. Meshtastic's and MeshCore's own presets (SF11 at
  250 kHz, SF7 at 62.5 kHz in the US) were not run.
* **Other radios.** The SX1276 and LR1110 were not tested.
* One pair of boards, one room, one afternoon.
