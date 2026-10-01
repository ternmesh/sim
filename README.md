# Tern simulator

A discrete-event LoRa simulator, written in C17, for deciding Tern's routing design before any of it is specified. It compares managed flooding, path routing and distance-vector routing by **delivery per unit of airtime** at 50, 200 and 1000 nodes, on a radio model calibrated against bench measurements of real SX1262 radios (`calibration/`).

Part of **Tern**, a LoRa mesh protocol that treats airtime as a shared,
metered resource. The protocol is defined by the specification in
[ternmesh/spec](https://github.com/ternmesh/spec), not by this code.

**Status:** the core is being built. What exists today:

| Module | Header | What it is |
|---|---|---|
| Scheduler | `tsim/sched.h` | The clock and the event queue. Events at the same time run in scheduling order, so a run is a pure function of its inputs and seed. |
| Airtime | `tsim/lora.h` | LoRa time on air (Semtech's formula, SF7–SF12), in integer nanoseconds. |
| Randomness | `tsim/rng.h` | Seeded streams (xoshiro256\*\*), one per source of randomness, so a run is reproducible on every platform. |
| Channel | `tsim/channel.h` | Log-distance path loss with correlated shadowing: links whose ends stand near each other share part of their luck. |
| Radios | `tsim/phy.h` | Half-duplex radios tuned to one channel, SF and bandwidth at a time. Interference is summed per SF and weighted by overlap, a louder frame can take a receiver only during the preamble, and retuning costs deaf time. |
| Network | `tsim/net.h` | The seam a candidate plugs into: routing decides what is sent, the MAC decides when. Plugins see only what was on the air, every frame is charged to a per-node airtime ledger under its purpose (data, relay, control, announce), and each message is delivered at most once per destination. |
| Baseline | `tsim/baseline.h` | ALOHA with a random start, and naive flooding with a hop limit: the floor every candidate has to beat. |

The isolation matrix, capture threshold, lock time and retune time are placeholders from the
literature until the bench rig (MSH-32) measures them; they are parameters, meant to be swept.

Next: the traffic generator, the scenario format and the metrics, then the candidates.

* [CONTRIBUTING.md](CONTRIBUTING.md) — DCO sign-off and the clean-room rule
* [Governance](https://github.com/ternmesh/spec/blob/main/GOVERNANCE.md)

## Building

Linux or macOS, with CMake 3.20 or later and a C17 compiler (on Windows, use WSL).

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

`-DTSIM_SANITIZE=ON` builds with AddressSanitizer and UndefinedBehaviorSanitizer, and
`-DTSIM_WERROR=ON` makes warnings errors; CI runs both, with GCC and Clang. Format with
`clang-format` 18 before pushing.

## Licence

[Apache License 2.0](LICENSE). See [NOTICE](NOTICE).
