# FlexNode

**A distributed compute node for mobile robots — high-current FOC actuation and sensor fusion, all over CAN-FD.**

> ⚠️ **Work in progress — first article booted 2026-09-06, first motor spin under FOC 2026-09-08.** A v1.0 board powers up, boots from flash, runs the FlexNode firmware, and has driven a GIM 8108-8 open-loop from its own power stage (DRV8353S, all three phase-current channels, bus-voltage sense and FET thermals validated). Still unvalidated: closed-loop FOC and calibration (encoder not yet fitted), the phase-order fix on hardware, CAN enumeration, ToF and aux current sense. Expect breaking changes until the first article is fully proven.

---

## The vision

Building a legged robot today means building three projects: the robot, its electronics, and the firmware to glue them together. FlexNode's goal is to delete the last two.

**Buy FlexNodes + actuators + sensors. Wire power and CAN. Done — no electronics development.**

Each FlexNode is one node per joint: it closes a hard-real-time field-oriented control loop locally (15–30 kHz), reads its own IMU and time-of-flight sensor, and exposes ports for whatever the joint needs — an external RC servo, an AS5600L joint encoder, a load cell for foot contact, addressable status LEDs. Everything reports and commands over a single daisy-chained CAN-FD pair. One firmware image serves every node; per-node behavior is *configuration*, not code. A host — a Jetson, a laptop, a Raspberry Pi — composes queries per node and gets exactly the telemetry it asks for, at gait-loop rates.

The result: a 21-DoF sensor-fused quadruped's entire electronics stack is one identical board per BLDC actuator, two wires between them, and a config file. That is sweet for CATBOT (the robot this was built for), and just as sweet for anyone else building anything with motors and sensors on it.

## What it is

FlexNode is a compact 4-layer PCB that fuses a **field-oriented motor controller** and a **sensor-fusion front end** into a single CAN-FD bus node. It began as a fork of the [mjbots **moteus r4.11**](https://github.com/mjbots/moteus) controller — the proven power stage, gate driver, STM32G4 core, FOC firmware, and register protocol are kept intact — and adds the digital sensing and I/O a distributed robot node needs:

- **6-axis IMU** (accel + gyro) for per-node inertial sensing — one per node makes a distributed IMU array
- **Multizone time-of-flight** ranging (8×8) for proximity / terrain
- **Servo / Aux port** with onboard 5 V current sensing (stall detection)
- **Dedicated I²C port** for AS5600L-class joint encoders or any I²C peripheral
- **SPI pads** with spare GPIOs as chip-selects for SPI peripherals
- **Addressable RGB status** (WS2812)
- Everything on the **moteus CAN-FD register protocol**, extended with a FlexNode register block (`0x080–0x0FF`) — one universal map, per-node-composed queries, host-side fleet autodiscovery

## Core specs (v1.0)

| Domain | Part / value |
|---|---|
| MCU | STM32G473CEU6 / G474CEU6 — Cortex-M4F @170 MHz, 512 KB flash, 128 KB RAM, 5× ADC, 6× op-amp, 3× FDCAN, UFQFPN48 |
| Gate driver | TI DRV8353S (3-phase, SPI-configurable) |
| Power FETs | BSC016N06NS-class — 60 V, 1.6 mΩ, TDSON-8 |
| Motor supply | Up to ~44 V bus (10S LiPo class) |
| Aux rail | LGS5160C sync buck (→ 5 V @ 3 A) + ME6216A33M3G LDO (→ 3.3 V) |
| Rotor feedback | AS5047P magnetic encoder (SPI, on-axis) |
| Comms | CAN-FD 1M/5M via TCAN334G, daisy-chain in/out, leaf-node 120 Ω termination |
| Protocol | moteus register protocol + FlexNode block `0x080–0x0FF` — see [`docs/can-layer.md`](docs/can-layer.md) |
| IMU | ST LSM6DS3TR-C (I²C) |
| ToF | ST VL53L7CX 8×8 multizone (I²C) |
| Status | WS2812B addressable LED |
| Aux I/O | Servo / LED output + 5 V shunt current sense + I²C port + dual CAN ports, all JST-PH |
| Stackup | 4-layer, 1 oz copper (thermally validated for CATBOT's real phase currents; go 2 oz for full moteus current capability) |

## Firmware

Firmware is a fork of moteus, retaining its FOC inner loop, CAN protocol, and flash/bootloader layout. The compatibility layer is **implemented and building**; full deltas in [`docs/firmware.md`](docs/firmware.md):

- **Board identity is hardcoded** to `{family = 0, hw_version = 8}` (r4.11-class). FlexNode repurposes moteus's hardware-detection strap pins (PB11, PC6, PB10), so the runtime autodetection was **deleted outright** — boot never touches the repurposed pins, and `hw_version = 8` selects the correct DRV8353 register tables and r4.11 analog map for free.
- **Pin remaps**: `PC6 → AS5047 CS` (done) · `PB11 → 5 V current sense`, `PC13 → servo/LED`, `PB10 → ToF INT` (with peripheral bring-up).
- **Phase-order fix (implemented)**: the v1.0 copper swaps the A/C motor PWM outputs relative to moteus (PA0 → INHC, PA2 → INHA) while current-sense wiring stays stock — verified pin-by-pin against both schematics. Unfixed, the current loop would regulate the wrong winding. The fix is a **two-line drive-side pin remap** in the hardware table: the PWM CCR mapping follows the pins generically, so the entire control core stays byte-for-byte stock and calibration/`phase_invert` semantics are preserved.
- **Flash budget (measured)**: application image ≈ 408 KiB of the 446 KiB app window → **~36 KiB free** for FlexNode's additions — ample when reusing moteus's non-blocking I²C / SPI-DMA / FDCAN primitives and never blocking the FOC ISR.

## The distributed CAN layer

Designed and specified in [`docs/can-layer.md`](docs/can-layer.md). The short version:

- **Extend, don't invent**: moteus's register protocol already separates a *fixed register map* from *per-node-composed frames* — so every FlexNode shares one universal register layout (`0x080` capabilities bitmask, load cell, AS5600L, IMU, ToF, NeoPixel, servo blocks) while the host queries each node only for what it actually has.
- **One image, N configs**: node identity (CAN id, pin modes, encoder sources, peripheral enables) is persistent config. The whole fleet flashes the same `.elf`.
- **Two-lane cadence**, strictly poll-response: loop-critical data (foot contact, joint angles) rides the per-cycle command+query frame; everything else refreshes on a rotating ~25 Hz slow lane. Promoting a sensor to the fast lane is a host-side change only.
- **Phased host**: bring-up on an mjbots fdcanusb at 250–350 Hz on one chain; scale to 400 Hz–1 kHz by making CATBOT's core board an STM32G4 "spinal cord" driving two chains (the G4 has three FDCAN peripherals).

## Engineering highlights

Choices worth reading the docs for:

- **Buck inductor sized against the silicon, not the app note**: 10 µH at 400 kHz keeps peak inductor current (~3.55 A at full load) under the LGS5160C's 3.7 A high-side current limit — the smaller "typical" value would have current-limited at a third of the rated output.
- **Every BOM line datasheet-verified**: all 37 JLCPCB part numbers resolved and checked against their LCSC datasheets before fab (which caught stale schematic annotations that would otherwise read as fatal design errors).
- **Deleted, not bypassed, hardware autodetect**: less flash, and boot provably never drives the repurposed strap pins.
- **A two-line phase-topology fix**: re-pairing drive and sense windings via the pin table instead of touching control code — zero risk to the FOC core, verified against both netlists first.
- **On-node contact thresholding**: foot contact is computed on the node and read as a single byte in the fast lane — the cheapest possible loop-rate signal, and the seed of local reflexes later.
- **ToF's 84 KB init blob streamed over CAN at boot** instead of consuming flash the MCU doesn't have — the host feeds it through the moteus diagnostic tunnel.
- **Thermal design against real loads**: 1 oz 4-layer copper validated against CATBOT's actual phase-current profile (~2–5 A nominal, ~25 A stall transients) — the actuators thermally limit before the board does.

## About this project

FlexNode and CATBOT are **independently designed and built by [Aditya Dutta](https://thedutta.github.io)**, a third-year undergraduate at **Manipal Institute of Technology, Bengaluru** — no lab budget, no research group, no sponsor: one student, open-source tools, and JLCPCB.

**CATBOT** is the driving target: a 4.9 kg, 21-DoF, super-agile jumping quadruped — high-torque geared hips, lightweight leg motors, a Jetson for perception and locomotion policies, and one FlexNode per actuator fused into a whole-body sensor network. The ambition is deliberately at the edge of what an individual can build: dynamic, sensor-rich legged locomotion of the kind usually gated behind institutional hardware.

The method makes that possible: **stand on proven open source and extend it honestly.** moteus contributes a decade of motor-control engineering; FlexNode contributes the distributed-sensing node architecture, the CATBOT-specific hardware, and the documentation trail — and gives all of it back under the same license. Every schematic, BOM line, firmware delta, and design decision in this repo is public precisely so the next student can start where this project stands instead of where it started.

## Repository layout

FlexNode's files live at the repo root; the upstream **moteus r4** project this forks from is kept intact underneath, in `moteus-r4-parent/`:

```
<repo root>/                     ← FlexNode
├── README.md                    ← you are here
├── .gitattributes               LF pinning for WSL-executed scripts
├── docs/
│   ├── hardware.md              hardware architecture, pin map, power tree
│   ├── firmware.md              firmware port: deltas, phase fix, build/flash
│   ├── can-layer.md             distributed CAN layer: registers, cadence, bus budget
│   └── roadmap.md               status and next steps
├── hardware/
│   ├── FlexNode_schematics.pdf  v1.0 schematic (as ordered)
│   ├── CAD/                     3D model (STEP)
│   ├── manufacturing/           BOM, gerber archive, pick-and-place
│   └── gerbers/                 unpacked gerbers
├── LICENSE                      Apache-2.0 (inherited from moteus)
├── NOTICE                       attribution to moteus / mjbots, trademark note
└── moteus-r4-parent/            ← the moteus r4.11 fork FlexNode builds on
    ├── fw/ hw/ lib/ tools/ …    moteus firmware + build system
    ├── README.md                moteus's own readme
    └── LICENSE                  moteus's original Apache-2.0 (preserved)
```

> **Firmware lives in `moteus-r4-parent/`** and builds from *inside* that directory — its Bazel `WORKSPACE` is the build root. FlexNode's firmware deltas live in `moteus-r4-parent/fw/` (currently concentrated in `moteus_hw.cc`; see [`docs/firmware.md`](docs/firmware.md)).
>
> **Upstream sync:** moteus is deliberately relocated off the repo root so FlexNode is the top-level project, which means `git merge upstream/main` no longer applies cleanly. To pull upstream moteus fixes, `git fetch upstream` and copy/patch the changes into `moteus-r4-parent/` by hand. That manual sync is the accepted trade for owning the root.

## Status

See [`docs/roadmap.md`](docs/roadmap.md) for the live checklist. In short: **first article assembled and booted (2026-09-06) · power tree, flash boot, status LED and IMU validated · first spin 2026-09-08: power stage, phase-current sense, bus sense and FET thermals validated with open-loop drive · CAN layer designed, not yet implemented · encoder fit, closed-loop FOC, phase-order verification and CAN bring-up next.**

## Credits & license

FlexNode derives from the **mjbots moteus r4.11** open-hardware controller by Josh Pieper ([mjbots/moteus](https://github.com/mjbots/moteus), Apache-2.0). The power stage, gate-driver topology, STM32G4 core, FOC firmware, and CAN register protocol follow that project; the sensing suite, distributed-node architecture, CATBOT-specific hardware, and firmware deltas are FlexNode's.

This repository is licensed **Apache-2.0** (`LICENSE` at the root), inherited from moteus and covering FlexNode's additions unless a specific file states otherwise. Attribution and modification notices required by Apache §4 are collected in `NOTICE`; moteus's original license is preserved in `moteus-r4-parent/LICENSE`. Retain the moteus attribution and the upstream link.

"moteus" and "mjbots" are trademarks of mjbots Robotic Systems LLC. FlexNode is **moteus-compatible** (speaks the moteus register protocol and works with `moteus_tool` and fdcanusb) and is not endorsed by or affiliated with mjbots.

Author: **Aditya Dutta** · Manipal Institute of Technology, Bengaluru · Project: **CATBOT**

## Build notes

Working notes for the firmware and bench (what is validated, the bench rules, how to build/flash/debug, and lessons learned) live in [`notes/`](notes/). Read [`notes/README.md`](notes/README.md) first. Bench tooling is in [`tools/bench/`](tools/bench/).
