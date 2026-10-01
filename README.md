# Tern simulator

A discrete-event LoRa simulator, written in C17, for deciding Tern's routing design before any of it is specified. It compares managed flooding, path routing and distance-vector routing by **delivery per unit of airtime** at 50, 200 and 1000 nodes, on a radio model calibrated against bench measurements of real SX1262 radios (`calibration/`).

Part of **Tern**, a LoRa mesh protocol that treats airtime as a shared,
metered resource. The protocol is defined by the specification in
[ternmesh/spec](https://github.com/ternmesh/spec), not by this code.

**Status:** not started.

* [CONTRIBUTING.md](CONTRIBUTING.md) — DCO sign-off and the clean-room rule
* [Governance](https://github.com/ternmesh/spec/blob/main/GOVERNANCE.md)

## Licence

[Apache License 2.0](LICENSE). See [NOTICE](NOTICE).
