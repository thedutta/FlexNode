# FlexNode

**A distributed compute node for mobile robots — high-current FOC actuation and sensor fusion, all over CAN-FD.**

> ⚠️ **Work in progress.** FlexNode v1.0 boards have been fabricated; hardware bring-up and the firmware port are in progress. Schematics, BOM, and gerbers here are the as-ordered v1.0 design. Expect breaking changes until the first article is validated.

---

## What it is

FlexNode is a compact PCB that fuses a **field-oriented motor controller** and a **sensor-fusion front end** into a single node that hangs off a **CAN-FD** bus. Many nodes daisy-chain back to a host (a Jetson on the target robot), so each limb or joint gets local, hard-real-time control while the host does high-level planning.

It began as a fork of the [mjbots **moteus r4.11**](https://github.com/mjbots/moteus) controller — the proven power stage, gate driver, and STM32G4 core are kept intact — and adds the digital sensing and I/O a distributed robot node needs:

- **6-axis IMU** (accel + gyro) for per-node inertial sensing
- **Multizone time-of-flight** ranging (8×8) for proximity / terrain
- **Servo / Aux port** with onboard 5 V current sensing
- **Dedicated i2c port** for secondary encoders or other i2c peripherals
- **SPI pads** for spi peripherals using existing gpios on ports as cs lines
- **Addressable RGB status** (WS2812)
- Everything reported and commanded over **CAN-FD**

The design target is **CATBOT**, a 4.1 kg ultra-nimble jumping quadruped: FlexNode drives its high-torque geared hip actuators and its lighter leg motors, one node per actuator, all chained to the onboard Jetson, all either JST-PH or directly soldered, no space for XT90s.

## Core specs (v1.0)

| Domain | Part / value |
|---|---|
| MCU | STM32G473CEU6 / G474CEU6 — Cortex-M4F @170 MHz, 512 KB flash, 128 KB RAM, 5× ADC, 6× op-amp, 3× FDCAN, UFQFPN48 |
| Gate driver | TI DRV8353S (3-phase, SPI-configurable) |
| Power FETs | Infineon BSC016N06NS — 60 V, 1.6 mΩ, TDSON-8 |
| Motor supply | Up to ~44 V bus (10S LiPo class) |
| Aux rail | LGS5160C sync buck (→ 5 V) + MCP1700 LDO (→ 3.3 V) |
| Rotor feedback | AS5047P magnetic encoder (SPI, on-axis) |
| Comms | CAN-FD via TCAN334G, daisy-chain in/out, leaf-node 120 Ω termination |
| IMU | ST LSM6DS3TR-C (I²C) |
| ToF | ST VL53L7CX 8×8 multizone (I²C) |
| Status | WS2812B addressable LED |
| Aux I/O | Servo / LED output + 5 V shunt current sense + i2c programmable port + dual can ports , all JST-PH|
| Stackup | 4-layer, 1 oz copper (thermally validated for CATBOT's real phase currents, upgrade to 2oz on all layers for moteus current capability) |

## Firmware

Firmware is a fork of moteus, retaining its FOC inner loop and CAN protocol while remapping the pins FlexNode reuses and adding the sensor/aux features. Key deltas from stock moteus are documented in [`docs/firmware.md`](docs/firmware.md); the highlights:

- **Board ID is hardcoded** to `{family = 0, hw_version = 8}` — FlexNode repurposes the strap pins (PB11, PC6), so runtime hardware detection is bypassed. `hw_version = 8` selects the correct DRV8353 register tables and r4.11 analog map.
- **Pin remaps**: `PB11 → 5 V current sense`, `PC6 → AS5047 CS`, `PC13 → servo/LED`.
- **Phase-order fix**: the motor PWM phase A/C outputs are swapped in copper relative to moteus while the current-sense wiring matches moteus; firmware compensates by swapping the current-sense channel assignment (phase-0 ↔ phase-2) so drive and sense stay on the same physical phase. See [`docs/firmware.md`](docs/firmware.md#phase-order).
- **Flash budget**: the moteus application image is ~410 KiB; on the 512 KB part that leaves ~35 KiB for FlexNode's added features — ample if they reuse moteus's non-blocking I²C / SPI-DMA / FDCAN primitives and never block the FOC ISR.

## Repository layout

This repository is a **fork of mjbots/moteus** — the firmware and its full history live at the repo root, and everything FlexNode-specific lives under `flexnode/`:

```
<repo root>/                  ← fork of mjbots/moteus (firmware + upstream history)
├── fw/ hw/ lib/ …            moteus firmware, upstream (pull fixes from mjbots)
├── LICENSE  README.md        moteus's own (Apache-2.0)
└── flexnode/                 ← FlexNode
    ├── README.md             ← you are here
    ├── docs/
    │   ├── hardware.md        hardware architecture, pin map, power tree
    │   ├── firmware.md        firmware port: remaps, hw_version, build/flash
    │   └── roadmap.md         status and next steps
    └── hardware/
        ├── FlexNode_schematics.pdf   v1.0 schematic (as ordered)
        ├── CAD/                      3D model (STEP)
        ├── manufacturing/            BOM, gerber archive, pick-and-place
        └── gerbers/                  unpacked gerbers
```

> The moteus firmware is not vendored separately — it **is** this repository (upstream: mjbots/moteus). Pull upstream fixes with `git fetch upstream && git merge upstream/main`.

## Status

FlexNode is early. See [`docs/roadmap.md`](docs/roadmap.md) for the live checklist. In short: **PCBs fabricated, assembly and bring-up pending, firmware port underway.** Nothing here has been validated on hardware yet.

## Credits & license

FlexNode derives from the **mjbots moteus r4.11** open-hardware controller by Josh Pieper ([mjbots/moteus](https://github.com/mjbots/moteus), Apache-2.0). The power stage, gate-driver topology, and STM32G4 core follow that design; the sensing, CAN-FD node architecture, and firmware deltas are FlexNode's.

This repository inherits moteus's **Apache-2.0** `LICENSE` at its root, which covers the fork including FlexNode's additions unless a specific file states otherwise. Retain the moteus attribution and the upstream link.

Author: **Aditya Dutta** · Project: **CATBOT**
