# moteus-r4-parent

This folder is the **[mjbots moteus](https://github.com/mjbots/moteus) r4.11** project that FlexNode forks from — kept intact so FlexNode inherits its proven power stage, gate-driver handling, STM32G4 core, FOC firmware, and CAN protocol. Everything here except FlexNode's own firmware deltas is upstream moteus.

## Why it's a subfolder
The repo root belongs to **FlexNode** (see [`../README.md`](../README.md)). moteus lives here underneath as the parent it builds on. The full moteus git history is preserved in this repository (this is a fork of mjbots/moteus, with the tree relocated into this folder).

## Building the firmware
Build from **inside this folder** — its Bazel `WORKSPACE` is the build root:

```bash
cd moteus-r4-parent
tools/bazel build --config=target //:target
```

FlexNode's firmware changes go into `fw/` here. The specific deltas from stock moteus (board-ID hardcode, pin remaps, the phase-order fix) are documented in [`../docs/firmware.md`](../docs/firmware.md).

## Upstream
Upstream: https://github.com/mjbots/moteus (remote `upstream`). Because this tree was moved off the repo root, `git merge upstream/main` will not apply cleanly — pull upstream fixes by fetching `upstream` and copying/patching changes into this folder manually.

## License
moteus is Apache-2.0; its original `LICENSE` is preserved in this folder. The repository-level `LICENSE` at the root is the same Apache-2.0.
