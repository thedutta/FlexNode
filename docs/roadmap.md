# FlexNode — Roadmap & Status

_Last updated: 2026-09-06 (first article assembled, booted and flashed; WS2812 status/lighting driver live)_

FlexNode v1.0 **booted for the first time on 2026-09-06**: a board on a 13 V bench adapter, flashed over SWD, running the FlexNode firmware with a live status LED. Validated so far: power tree, MCU boot from flash, SWD/flash toolchain, PF0 → WS2812B, moteus main loop. Everything below that is still unvalidated — no motor and no encoder are fitted, and the gate driver has never been energized.

## Hardware

- [x] v1.0 schematic — power stage, gate driver, MCU, CAN-FD, sensors
- [x] Independent schematic review vs moteus r4.11 reference
- [x] Copper-weight / thermal analysis for CATBOT's real currents (1 oz, 4-layer)
- [x] MCU sourcing resolved (512 KB UFQFPN48 only: G474CEU6 / G473CEU6; buy spares for QFN attrition)
- [x] BOM finalized + LCSC part matching
- [x] Gerbers + pick-and-place exported; boards ordered
- [x] PCBA / assembly — first article built (encoder not yet soldered)
- [x] Power-on smoke test — 13 V bench adapter, 5 V buck and 3.3 V logic rail stable, no heating
- [ ] Gate-driver + FET bring-up, current-sense calibration (not energized yet)

## Firmware

- [x] Fork moteus; `DetectMoteusFamily()` → hardcoded `{family = 0, hw_version = 8}`; autodetect + n1/c1/x1 pin maps deleted; builds green (429,208 B app, ~25 KiB free)
- [x] AS5047 CS remap PB11 → PC6
- [x] **Phase-order fix**: netlist-verified drive-side A/C re-pair (`pwm1`/`pwm3` pin swap; control core untouched) — see [firmware.md](firmware.md#phase-order); hardware validation at bring-up
- [x] Remove secondary-encoder / strap-pin references (gone with the autodetect deletion; encoder-source config set per node at bring-up)
- [ ] Remaining pin bring-up: PB11 → 5 V sense ADC, PC13 → servo/LED, PB10 → ToF INT
- [ ] Encoder bring-up on new CS pin (AS5047 not yet soldered); FOC calibration
- [ ] IMU (LSM6DS3TR-C) driver over non-blocking I²C
- [ ] ToF (VL53L7CX) driver + INT handling
- [x] **WS2812B status / master-control lighting** (`fw/ws2812_led.{h,cc}`, `led.*` config) — DWT-cycle-timed bit-bang from the main loop (PF0 has no usable SPI/timer AF); pixel 0 blinks fault codes, pixels 1..N are external lighting. First light 2026-09-06.
- [ ] Servo output (timer PWM) + 5 V current-sense monitor
- [x] CAN layer designed — register block 0x080–0x0FF, one-image + per-node config, poll-response two-lane cadence: see [can-layer.md](can-layer.md)
- [ ] FlexNode register-block handlers (0x080–0x0FF) in `moteus_controller.cc` + `flexnode.*` config structs
- [ ] `kAs5600L` I²C device type (programmable address) → Encoder-2 fast-lane path
- [ ] Load-cell path (analog-amp vs HX711 decision) + on-node contact thresholding (reg 0x08B)
- [ ] ToF init-blob streaming over the diagnostic tunnel (~84 KB, can't live in flash) + summary registers
- [x] Confirm `nBOOT0` option byte = boot-from-flash — `nSWBOOT0` cleared so the I²C pull-up on PB8/BOOT0 cannot force the bootloader
- [ ] First-article flash-write / config-persist validation (`0x0807f000` write + power-cycle)
- [ ] CAN enumeration + telemetry; `bus_V` vs DMM check (validates the as-built R30 rescale)

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
