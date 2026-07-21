# FlexNode — Roadmap & Status

_Last updated: 2026-07-21 (firmware compat layer implemented + builds; CAN layer designed; docs overhauled)_

FlexNode v1.0 is **pre-bring-up**. Boards are fabricated; nothing below the "hardware" line has been validated on physical hardware yet.

## Hardware

- [x] v1.0 schematic — power stage, gate driver, MCU, CAN-FD, sensors
- [x] Independent schematic review vs moteus r4.11 reference
- [x] Copper-weight / thermal analysis for CATBOT's real currents (1 oz, 4-layer)
- [x] MCU sourcing resolved (512 KB UFQFPN48 only: G474CEU6 / G473CEU6; buy spares for QFN attrition)
- [x] BOM finalized + LCSC part matching
- [x] Gerbers + pick-and-place exported; boards ordered
- [ ] PCBA / assembly
- [ ] Power-on smoke test (aux rails 5 V / 3.3 V, no board heating)
- [ ] Gate-driver + FET bring-up, current-sense calibration

## Firmware

- [x] Fork moteus; `DetectMoteusFamily()` → hardcoded `{family = 0, hw_version = 8}`; autodetect + n1/c1/x1 pin maps deleted; builds green (417,600 B app, ~36 KiB free)
- [x] AS5047 CS remap PB11 → PC6
- [x] **Phase-order fix**: netlist-verified drive-side A/C re-pair (`pwm1`/`pwm3` pin swap; control core untouched) — see [firmware.md](firmware.md#phase-order); hardware validation at bring-up
- [x] Remove secondary-encoder / strap-pin references (gone with the autodetect deletion; encoder-source config set per node at bring-up)
- [ ] Remaining pin bring-up: PB11 → 5 V sense ADC, PC13 → servo/LED, PB10 → ToF INT
- [ ] Encoder bring-up on new CS pin; FOC calibration
- [ ] IMU (LSM6DS3TR-C) driver over non-blocking I²C
- [ ] ToF (VL53L7CX) driver + INT handling
- [ ] WS2812 status via SPI/timer-DMA
- [ ] Servo output (timer PWM) + 5 V current-sense monitor
- [x] CAN layer designed — register block 0x080–0x0FF, one-image + per-node config, poll-response two-lane cadence: see [can-layer.md](can-layer.md)
- [ ] FlexNode register-block handlers (0x080–0x0FF) in `moteus_controller.cc` + `flexnode.*` config structs
- [ ] `kAs5600L` I²C device type (programmable address) → Encoder-2 fast-lane path
- [ ] Load-cell path (analog-amp vs HX711 decision) + on-node contact thresholding (reg 0x08B)
- [ ] ToF init-blob streaming over the diagnostic tunnel (~84 KB, can't live in flash) + summary registers
- [ ] Confirm `nBOOT0` option byte = boot-from-flash
- [ ] First-article flash-write / config-persist validation

## System (CATBOT integration)

- [ ] One node per actuator: 4× GIM8108-8 geared hips + leg 5010s
- [ ] Daisy-chain bring-up; terminate only the two leaf nodes (120 Ω bridge)
- [ ] Host (Jetson) ↔ node protocol; sensor-fusion pipeline — design in [can-layer.md](can-layer.md) (Phase A: fdcanusb @ 250–350 Hz; Phase B: core-board STM32G4 dual-chain bridge @ 400 Hz+)
- [ ] Multi-node time-sync / control-rate budgeting on the shared bus — budget tables in [can-layer.md](can-layer.md); sync deferred until IMU fusion demands it

## Known open items

- Gate-drive currents not re-tuned for BSC016N06NS (inherited r4.11 settings) — watch switching edges/EMI on first article.
- 5 V current sense is coarse and rail-referenced — firmware zero-offset calibration needed.
- External motor-temperature sensing (VTEMP_MOT / PA8) has a pull-up but no populated NTC connector — confirm intent.
- FlexNode's own `LICENSE` not yet set — required before any public release (retain moteus/Apache-2.0 attribution).
