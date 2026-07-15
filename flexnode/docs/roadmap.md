# FlexNode — Roadmap & Status

_Last updated: 2026-07-15_

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

- [ ] Fork moteus; set `DetectMoteusFamily()` → `{family = 0, hw_version = 8}`
- [ ] Pin remaps: PB11 → 5 V sense, PC6 → AS5047 CS, PC13 → servo/LED, PB10 → ToF INT
- [ ] **Phase-order fix**: swap current-sense phase-0 ↔ phase-2 (A↔C) to match swapped PWM — see [firmware.md](firmware.md#phase-order)
- [ ] Remove secondary-encoder references
- [ ] Encoder bring-up on new CS pin; FOC calibration
- [ ] IMU (LSM6DS3TR-C) driver over non-blocking I²C
- [ ] ToF (VL53L7CX) driver + INT handling
- [ ] WS2812 status via SPI/timer-DMA
- [ ] Servo output (timer PWM) + 5 V current-sense monitor
- [ ] CAN-FD reporting/command schema for the added telemetry (IMU, ToF, aux current)
- [ ] Confirm `nBOOT0` option byte = boot-from-flash
- [ ] First-article flash-write / config-persist validation

## System (CATBOT integration)

- [ ] One node per actuator: 4× GIM8108-8 geared hips + leg 5010s
- [ ] Daisy-chain bring-up; terminate only the two leaf nodes (120 Ω bridge)
- [ ] Host (Jetson) ↔ node protocol; sensor-fusion pipeline
- [ ] Multi-node time-sync / control-rate budgeting on the shared bus

## Known open items

- Gate-drive currents not re-tuned for BSC016N06NS (inherited r4.11 settings) — watch switching edges/EMI on first article.
- 5 V current sense is coarse and rail-referenced — firmware zero-offset calibration needed.
- External motor-temperature sensing (VTEMP_MOT / PA8) has a pull-up but no populated NTC connector — confirm intent.
- FlexNode's own `LICENSE` not yet set — required before any public release (retain moteus/Apache-2.0 attribution).
