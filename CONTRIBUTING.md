# Contributing to ternmesh/sim

Thank you for helping. This repository is one part of Tern; how the
project is governed, and who decides what, is in
[GOVERNANCE.md](https://github.com/ternmesh/spec/blob/main/GOVERNANCE.md)
in the specification repository.

## Sign your commits (DCO)

Tern uses the [Developer Certificate of Origin](DCO) instead of a
contributor licence agreement. You keep the rights to your work; you
certify that you are allowed to contribute it under this repository's
licence.

Add a `Signed-off-by` line to every commit with your real name and an
email address you control:

```bash
git commit -s -m "Describe the change"
```

Pull requests with unsigned commits cannot be merged. To sign commits
you have already made: `git rebase --signoff main`.

## Licence

Contributions are licensed under the [Apache License 2.0](LICENSE), which
includes an explicit patent grant from every contributor.

## The specification comes first

This repository implements the specification in
[ternmesh/spec](https://github.com/ternmesh/spec). A change that alters
protocol behaviour starts there, as a specification change with test
vectors, and is implemented here afterwards.

## Clean-room rule

Do not copy source code from other mesh implementations (Meshtastic,
MeshCore, Reticulum or their ports) into this repository, even where the
licence would appear to allow it. Work from published documentation and
observed packets.
