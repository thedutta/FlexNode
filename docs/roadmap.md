# FlexNode — Roadmap & Status

_Last updated: 2026-09-08 (first spin: power stage and sensing validated open-loop; CAN layer v2 and flash budget measured)_

FlexNode v1.0 **booted for the first time on 2026-09-06** and **drove a motor for the first time on 2026-09-08**: a GIM 8108-8 turning open-loop from a current-limited 13 V / 1 A supply, on a throwaway bench image (`bench/openloop-test`). Validated on hardware so far: power tree, boot from flash, SWD/flash toolchain, WS2812B status LED, LSM6DS3TR-C IMU, DRV8353S enable, six-FET power stage under load, all three phase-current channels, bus-voltage sense against a DMM, FET thermistor, and the bus-sag abort path. **Not yet:** the encoder is unsoldered, so no closed-loop FOC, calibration or position control has run; the phase-order fix is unverified (open-loop drive cannot test it); CAN has never been exercised. Details in [`notes/bringup-log.md`](../notes/bringup-log.md).

## Hardware

- [x] v1.0 schematic — power stage, gate driver, MCU, CAN-FD, sensors
- [x] Independent schematic review vs moteus r4.11 reference
- [x] Copper-weight / thermal analysis for CATBOT's real currents (1 oz, 4-layer)
- [x] MCU sourcing resolved (512 KB UFQFPN48 only: G474CEU6 / G473CEU6; buy spares for QFN attrition)
- [x] BOM finalized + LCSC part matching
- [x] Gerbers + pick-and-place exported; boards ordered
- [x] PCBA / assembly — first article built (encoder not yet soldered)
- [x] Power-on smoke test — 13 V bench adapter, 5 V buck and 3.3 V logic rail stable, no heating
- [x] Gate-driver + FET bring-up, current-sense calibration — open-loop first spin 2026-09-08 (bench branch); closed-loop and phase-order check still pending on the encoder

## Firmware

- [x] Fork moteus; `DetectMoteusFamily()` → hardcoded `{family = 0, hw_version = 8}`; autodetect + n1/c1/x1 pin maps deleted; builds green (437,232 B app, ~17 KiB free)
- [x] AS5047 CS remap PB11 → PC6
- [x] **Phase-order fix**: netlist-verified drive-side A/C re-pair (`pwm1`/`pwm3` pin swap; control core untouched) — see [firmware.md](firmware.md#phase-order); hardware validation at bring-up
- [x] Remove secondary-encoder / strap-pin references (gone with the autodetect deletion; encoder-source config set per node at bring-up)
- [ ] Remaining pin bring-up: PB11 → 5 V sense ADC, PC13 → servo/LED, PB10 → ToF INT
- [ ] Encoder bring-up on new CS pin (AS5047 not yet soldered); FOC calibration
- [x] **IMU (LSM6DS3TR-C)** as aux2 I²C device type (`kLsm6ds3`, on by default on FlexNode), regs 0x098–0x09F — bench-verified 2026-09-06 via the LED tilt demo
- [ ] ToF (VL53L7CX) driver + INT handling
- [x] **WS2812B status / master-control lighting** (`fw/ws2812_led.{h,cc}`, `led.*` config) — DWT-cycle-timed bit-bang from the main loop (PF0 has no usable SPI/timer AF); pixel 0 blinks fault codes, pixels 1..N are external lighting. First light 2026-09-06.
- [ ] Servo output (timer PWM) + 5 V current-sense monitor
- [x] CAN layer designed — register block 0x080–0x0FF, one-image + per-node config, poll-response two-lane cadence: see [can-layer.md](can-layer.md)
- [x] FlexNode register-block skeleton in `moteus_controller.cc`: 0x080/0x081, IMU, pixel 0x0B0–0x0B4, 0x0FF (unverified over CAN)
- [ ] Register block: remaining peripherals (load cell, AS5600L raw, ToF, servo) + `flexnode.*` config structs; verify over CAN once an adapter is on the bench
- [ ] `kAs5600L` I²C device type (programmable address) → Encoder-2 fast-lane path
- [ ] Load-cell path (analog-amp vs HX711 decision) + on-node contact thresholding (reg 0x08B)
- [ ] ToF init-blob streaming over the diagnostic tunnel (~84 KB, can't live in flash) + summary registers
- [x] Confirm `nBOOT0` option byte = boot-from-flash — `nSWBOOT0` cleared so the I²C pull-up on PB8/BOOT0 cannot force the bootloader
- [ ] First-article flash-write / config-persist validation (`0x0807f000` write + power-cycle)
- [x] `bus_V` vs DMM check — 2026-09-08, 12.82 V vs 12.50 V, `vsense_adc_scale` trimmed on the bench branch (port to `main` pending)
- [ ] CAN enumeration + telemetry (needs a CAN-FD adapter)

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
