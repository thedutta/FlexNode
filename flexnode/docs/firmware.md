# FlexNode — Firmware

FlexNode's firmware is a fork of [mjbots/moteus](https://github.com/mjbots/moteus). The FOC inner loop, FDCAN register protocol, and flash/bootloader layout are kept; only the pins FlexNode reuses are remapped, plus the added sensor/aux features. This document tracks the deltas from stock moteus.

> **WIP.** The port is not yet complete or hardware-validated. Treat the code pointers below as the intended changes, to be confirmed on the first article.

## Board identity — hardcode `{family = 0, hw_version = 8}`

FlexNode repurposes the strap pins moteus uses for hardware detection (PB11, PC6), so runtime family/version detection would misread. `DetectMoteusFamily()` (in `fw/moteus_hw.cc`) must return **`{family = 0, hw_version = 8}`**.

Why `8` specifically:
- `fw/drv8323.cc` selects DRV8323-vs-**DRV8353** register tables at runtime by `hw_version`: `>= 7` → DRV8353 (r4.8/r4.10/r4.11). FlexNode's gate driver is the DRV8353S, so it needs the `>= 7` path for correct `IDRIVEP/IDRIVEN/DEGLITCH/VDS_LVL`, plus `OCP_ACT` and `CAL_MODE`.
- `hw_version = 8` also selects the correct r4.11 analog map: `vsense = PB_12_ALT0`, `msense = PA_8`, `vsense_adc_scale = 0.017947`.

`hw_version = 0` would program the DRV8353 with DRV8323 codes (wrong slew/OCP/current-sense calibration) and the wrong analog map — do not use it.

## Pin remaps

| Signal | moteus pin | FlexNode pin | Firmware touch-point |
|---|---|---|---|
| AS5047 chip-select | PB11 | **PC6** | encoder SPI CS config |
| 5 V (servo) current sense | — | **PB11** | add ADC channel (aux monitor) |
| Servo / LED output | PC13 | **PC13** | aux PWM/GPIO (was secondary-encoder CS) |
| ToF interrupt | — | **PB10** | GPIO EXTI for VL53L7CX |
| WS2812 data | PF0 | **PF0** | SPI/timer-DMA driver |

The secondary encoder is omitted; remove/disable its references.

## <a name="phase-order"></a>Phase-order fix (must-do)

On the FlexNode PCB the **motor PWM phase A/C outputs are swapped** relative to moteus, but the **current-sense wiring is identical to moteus** (only the schematic net *labels* were renamed). Verified from the v1.0 netlist:

| | Phase A drive | Phase A sense | Phase C drive | Phase C sense |
|---|---|---|---|---|
| moteus | PA0 → INHA | SOA → PB0 | PA2 → INHC | SOC → PB2 |
| FlexNode | **PA2** → INHA | SOA → PB0 | **PA0** → INHC | SOC → PB2 |

Stock moteus pairs the PWM channel on PA0 with the current-sense ADC on PB0 as the *same* phase. On FlexNode that pairing drives physical phase **C** while sensing physical phase **A** — the current loop would close on the wrong winding (not a direction reversal; calibration can't fix it).

**Fix (firmware, no respin):** restore drive/sense consistency by swapping the A↔C assignment on **one** side only —
- swap the current-sense channel assignment so the ADC on **PB0 is treated as phase C** and **PB2 as phase A** (phase-0 ↔ phase-2), **or**
- swap the two PWM channel-to-phase assignments (drive phase A on PA2's channel, phase C on PA0's).

Either re-pairs drive-C with sense-C. Do exactly one. Confirm against the ECAD before flashing (PA0 → INHC? PB0 ← SOA?).

## Real-time constraints

The FOC current loop runs in a tens-of-kHz ISR. Added features must never block it:
- **WS2812**: clock the 800 kHz stream via SPI-DMA or timer-DMA; don't bit-bang in the ISR path.
- **I²C sensors (IMU/ToF)**: use moteus's non-blocking I²C (`fw/stm32_i2c.h`, the aux-port pattern); poll from the slow loop.
- **Servo**: drive from a timer PWM channel (50 Hz), trivial cost.

## Flash budget

Measured (moteus HEAD, fw 0x000105, repo-pinned Bazel in WSL Ubuntu-22.04):

- moteus application image ≈ **409.5 KiB** (`.text` 376 KiB + `.ccmram` init 30 KiB + `.data` 2.7 KiB), `0x8010000 → 0x8076610`.
- On the **512 KB** part (G473CEU6 / G474CEU6): app window to config = 446 KiB → ~**35 KiB free**.
- FlexNode's additions fit that headroom if written lean and reusing existing moteus primitives (FDCAN, `fw/pid.h`, `fw/stm32_spi.h`, non-blocking I²C).
- ⚠️ 256 KB parts (`…CCU6`) **do not fit** (~2.2× over) — MCU must be a 512 KB UFQFPN48 (`…CEU6`) with the full 5-ADC performance-line analog. LQFP48 parts are **package-incompatible** (no PC4/PC6). See project notes.

## Build & flash

```bash
# in WSL (Ubuntu-22.04), from the moteus fork root
tools/bazel build --config=target //:target      # repo-pinned Bazel 7.4.1
# flash via the moteus bootloader / SWD (P1 header: SWDIO=PA13, SWCLK=PA14, NRST)
```

First-article checklist before trusting flash writes: exercise the config-write path (`0x807f000`) with a power-cycle, and optionally confirm `DBANK = 1` via CubeProgrammer on the G473 die.

## Open firmware tasks

See [`roadmap.md`](roadmap.md).
