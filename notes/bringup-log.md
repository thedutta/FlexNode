# Bring-up log

Chronological, absolute dates. Newest at the bottom. Each entry says what was **validated on hardware**, not what was written.

## 2026-07-21 — compat layer
Board identity hardcode, autodetect deletion, AS5047 CS → PC6, drive-side phase re-pair (netlist-verified on paper). Builds green. Nothing run on hardware.

## 2026-07-24 — vsense rescale
As-built R30 = 1.2 kΩ (designer-confirmed, not 4.7 kΩ). Compensated in firmware, no rework. Needs a `bus_V` vs DMM check at first CAN contact.

## 2026-09-06 — first article boots (commits `5f32b08` → `ef0d948`)
Board on a 13 V bench adapter; 5 V and 3.3 V rails stable.
- SWD probe: STM32G47x, IDCODE `0x20036469`, 512 KiB dual-bank, RDP0, factory option bytes, blank flash.
- nSWBOOT0 cleared (boot from flash; PB8/BOOT0 sits high on the I²C pull-up). FLASH_OPTR now `0xFBEFF8AA`.
- Bare-metal 948 B WS2812 chime on PF0: **first light, first try.** Proves boot from flash, PF0 wiring, 16 MHz HSI on frequency.
- moteus FlexNode image flashed and running. Main loop healthy (PC sampling). Servo in `kStopped`.
- WS2812 status driver: correct colours confirmed (default soft blue), fault-code blink designed (not yet observed: no fault has occurred), **one-shot blue breath on OK then dark** confirmed after the `0`-bit timing fix.
- **LSM6DS3TR-C IMU** on aux2 I²C: WHO_AM_I answers, samples flow; verified by the tilt→hue demo with the board in hand. No CAN adapter involved.
- Register block skeleton (0x080/0x081, IMU, pixel, 0x0FF) compiled in. **Unverified over CAN.**
- Standalone power-up (ST-Link fully disconnected) confirmed working once the NRST rule was understood.
- Portfolio/site updated with the first-light photo by a subagent (site commits `1ea15c2`, `ae1d7af`, `8bfc4c6`).

Image at end of day: 437,808 B, ~16 KiB free.

## Still gated (as of 2026-09-06)
| Gate | Status | What unblocks it |
|---|---|---|
| CAN enumeration, telemetry, register block over the wire | not started | a CAN-FD adapter (fdcanusb or CANable-FD) |
| `bus_V` vs DMM (validates the R30 rescale) | not started | same adapter (`servo_stats.bus_V` over CAN) |
| Encoder bring-up on PC6 | not started | AS5047 soldered to the back side |
| Config-write path (0x0807F000) survives power cycle | not started | `conf write` over CAN, then power cycle |
| **Phase-order validation** | not started | current-limited supply, `moteus_tool --calibrate`, watch convergence and phase currents. **Hard gate before any motor drive.** |
| Bring-up config: `servo.max_current_A` 25–30, `servo.vds_lvl_mv` 100–200, `servo.max_voltage` ≈ 38 (8S) | not applied | CAN |
| Gate-drive currents for BSC016N06NS (inherited r4.11) | untested | first spin, watch edges/EMI |
