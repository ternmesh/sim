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
| Channel | `tsim/channel.h` | Log-distance path loss with correlated shadowing: links whose ends stand near each other share part of their luck. The 3GPP macro-cell models are there too, for reproducing Meshtasticator. |
| Radios | `tsim/phy.h` | Half-duplex radios tuned to one channel, SF and bandwidth at a time. Interference is summed per SF and weighted by overlap, a louder frame can take a receiver only during the preamble, and retuning costs deaf time. Per-frame fading is optional, and so are LoRaSim's simplifications - pairwise interference, capture at any time, a CAD margin and delay - for reproducing simpler simulators.
| Network | `tsim/node.h`, `tsim/net.h` | The seam a candidate plugs into: routing decides what is sent, the MAC decides when. A plugin is handed its own node (`node.h`) and nothing that reaches the radio model, the losses or another node; the driver builds the run and reads the books through `net.h`. Plugins see only what was on the air, every frame is charged to a per-node airtime ledger under its purpose (data, relay, control, announce), and a message is delivered at most once per destination, only by a node that received a frame verifiably carrying its content. |
| Baseline | `tsim/baseline.h` | ALOHA with a random start, and naive flooding with a hop limit: the floor every candidate has to beat. |
| Candidate 1 | `tsim/meshtastic.h` | Meshtastic's managed flood and its MAC, written from Meshtasticator's port of the firmware timing: SNR-weighted rebroadcast delays, cancelling a rebroadcast on hearing it from another node, implicit and real acknowledgements with retries, and a contention window that grows with channel utilisation. The compatibility mode that reproduces Meshtasticator's results comes next. |
| Traffic | `tsim/traffic.h`, `tsim/place.h` | Each node sends on its own Poisson process, with a configurable mix of broadcast and unicast and a range of lengths. The draws come from streams no plugin can seed, so with the same seed every candidate is offered the same messages at the same times. Nodes can be placed uniformly at random, on a grid or along a line. |
| Metrics | `tsim/metrics.h` | The headline is deliveries that arrive within the deadline per second of airtime. Alongside it: the busiest node's duty cycle, delivery and on-time ratios, latency percentiles, airtime by purpose, and collision counts. Unicast and broadcast are reported separately. |
| Scenarios | `tsim/scenario.h`, `tools/tsim.c` | A run written as a text file, which `tsim` runs and reports on as JSON. See [docs/scenarios.md](docs/scenarios.md). |

The isolation matrix, capture threshold, lock time and retune time are placeholders from the
literature until the bench rig (MSH-32) measures them; they are parameters, meant to be swept.

Next: candidate 1's Meshtasticator compatibility mode, then path routing (candidate 2) and
distance-vector routing (candidate 3).

* [CONTRIBUTING.md](CONTRIBUTING.md) — DCO sign-off and the clean-room rule
* [Governance](https://github.com/ternmesh/spec/blob/main/GOVERNANCE.md)

## Building

Linux or macOS, with CMake 3.20 or later and a C17 compiler (on Windows, use WSL).

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Then run a scenario:

```bash
./build/tsim scenarios/town.tsim
./build/tsim -s seed=2 -s routing.hops=5 scenarios/town.tsim
```

A 1000-node, one-hour run (`scenarios/scale/region.tsim`) takes a few seconds in a release build
(`-DCMAKE_BUILD_TYPE=Release`).

`-DTSIM_SANITIZE=ON` builds with AddressSanitizer and UndefinedBehaviorSanitizer, and
`-DTSIM_WERROR=ON` makes warnings errors; CI runs both, with GCC and Clang. Format with
`clang-format` 18 before pushing.

## Licence

[Apache License 2.0](LICENSE). See [NOTICE](NOTICE).
