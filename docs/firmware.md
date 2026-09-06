# FlexNode — Firmware

FlexNode's firmware is a fork of [mjbots/moteus](https://github.com/mjbots/moteus). The FOC inner loop, FDCAN register protocol, and flash/bootloader layout are kept byte-for-byte; FlexNode's deltas are concentrated in the hardware-description layer (`fw/moteus_hw.cc`) plus, later, the sensor/aux features. This document tracks the deltas from stock moteus.

> **First article booted 2026-09-06.** A v1.0 board ran the FlexNode firmware for the first time: powered from a 13 V bench adapter (no motor, no encoder fitted), flashed over SWD with an ST-Link V2, steady soft-blue status LED, servo in stopped mode.
>
> **Validated:** power tree (5 V buck and 3.3 V logic rail stable), MCU boots from flash, SWD/flash toolchain, PF0 → WS2812B, moteus main loop running.
> **Still pending:** CAN enumeration, `bus_V`-vs-DMM check (validates the as-built R30 rescale), encoder (not yet soldered), and anything that energizes the gate driver. Calibration and motor drive stay gated behind current-limited phase-order validation — see the checklist at the bottom.

## Status at a glance

| Delta | Status |
|---|---|
| Board identity hardcode `{family 0, hw_version 8}` | ✅ implemented, builds |
| Runtime hardware autodetection | ✅ deleted (strap pins are repurposed) |
| AS5047 CS remap PB11 → PC6 | ✅ implemented |
| WS2812B status/lighting driver on PF0 (`led.*` config) | ✅ implemented (first light 2026-09-06) |
| **Phase-order fix** (drive-side A/C re-pair) | ✅ implemented, **netlist-verified**; hardware validation pending |
| **Bus-voltage sense rescale** (as-built R30 = 1.2 k, not 4.7 k) | ✅ implemented — `vsense_adc_scale = 0.067944` (R30 value designer-confirmed); **DMM-verify at first boot** |
| n1/c1/x1 family pin maps | ✅ deleted (FlexNode is permanently family 0) |
| **LSM6DS3TR-C IMU** on aux2 I²C (PB8/PB9), on by default | ✅ implemented, **bench-verified 2026-09-06** (tilt → LED hue demo) |
| FlexNode CAN register block (0x080–0x0FF) handlers | 🟡 skeleton in `moteus_controller.cc`: 0x080/0x081, IMU 0x098–0x09F, pixel 0x0B0–0x0B4, 0x0FF — unverified over CAN (no adapter yet) |
| PB11 5 V-sense ADC · PC13 servo/LED · PB10 ToF INT · load cell | ⏳ with peripheral bring-up — design in [can-layer.md](can-layer.md) |

## Board identity — hardcoded `{family = 0, hw_version = 8}`

FlexNode repurposes the strap pins moteus uses for hardware detection (PB10, PB11, PC6), so runtime family/version detection would misread the board — and would briefly drive those repurposed pins during boot. `DetectMoteusFamily()` in `fw/moteus_hw.cc` now returns **`{family = 0, hw_version = 8, hw_pins = 4}`** unconditionally; the strap-reading / gate-driver-probing autodetection and the moteus n1/c1/x1 (family 1/2/3) pin maps have been **deleted outright** (≈1.7 KiB flash back, and boot provably never touches PB10/PB11).

Why `8` specifically — it is load-bearing twice:
- `fw/drv8323.cc` selects DRV8323-vs-**DRV8353** register tables at runtime by `hw_version >= 7`. FlexNode's gate driver is the DRV8353S, so this routes it to the correct `IDRIVEP/IDRIVEN/DEGLITCH/VDS_LVL` tables plus the DRV8353-only `OCP_ACT` and `CAL_MODE` bits — with **zero edits to drv8323.cc**.
- It selects the r4.11 analog map: `vsense = PB12`, `msense = PA8`, `vsense_adc_scale = 0.017947`.

## Pin remaps

| Signal | moteus pin | FlexNode pin | Status |
|---|---|---|---|
| AS5047 chip-select | PB11 | **PC6** | ✅ done (`moteus_hw.cc`) |
| Motor PWM phase A / C | PA0 / PA2 | **PA2 / PA0** | ✅ done — see phase-order fix below |
| 5 V (servo) current sense | — | **PB11** | ⏳ ADC channel + reg 0x082 |
| Servo / LED output | PC13 (2nd-enc CS) | **PC13** | ⏳ aux PWM/GPIO |
| ToF interrupt | — | **PB10** | ⏳ EXTI for VL53L7CX |
| WS2812 data | PF0 (debug LED) | **PF0** | ✅ `fw/ws2812_led.{h,cc}` — DWT-timed bit-bang from the main loop, IRQs masked only per high pulse; stock debug LED set NC |

The moteus secondary encoder is omitted; its strap/CS uses are gone with the autodetect deletion, and the encoder-source config is set per node at bring-up.

## <a name="phase-order"></a>Phase-order fix (implemented)

**Verified pin-by-pin against both schematic PDFs (2026-07-21).** The FlexNode v1.0 copper swaps the A/C motor PWM outputs relative to moteus, while current-sense connectivity is stock:

| Physical connection | moteus r4.11 | FlexNode v1.0 |
|---|---|---|
| PA0 → | MOTOR1 → INHA (winding A) | **MOTOR3 → INHC (winding C)** |
| PA1 → | MOTOR2 → INHB | MOTOR2 → INHB |
| PA2 → | MOTOR3 → INHC (winding C) | **MOTOR1 → INHA (winding A)** |
| PB0 ← | CUR1 ← SOA (winding A) | CUR3 ← SOA (winding A) — *same copper* |
| PB1 ← | CUR2 ← SOB | CUR2 ← SOB |
| PB2 ← | CUR3 ← SOC (winding C) | CUR1 ← SOC (winding C) — *same copper* |

Only the sense-side net *labels* (CUR1↔CUR3) were renamed in the schematic; connectivity is stock. ⚠️ **Bench-probing note: trust pins, not labels** — the net the FlexNode schematic calls `CUR3` is winding A and appears in firmware telemetry as `cur1_A`.

Unfixed, stock firmware would pair PA0's PWM channel (physically driving winding **C**) with PB0's ADC (physically sensing winding **A**) as one logical phase — the current loop closes on the wrong winding. That is a topology error: no calibration, offset, or sign flip can repair it, and driving a motor that way risks the FETs.

**The fix (implemented in `fw/moteus_hw.cc`, family-0 branch):**

```cpp
result.pwm1 = PA_2_ALT0;  // logical phase 1 drives PA2 -> INHA (winding A)
result.pwm3 = PA_0_ALT0;  // logical phase 3 drives PA0 -> INHC (winding C)
```

Two lines on the **drive side only**. `ConfigurePwmTimer()`/`FindCcr()` resolve timer CCR registers from these pins generically, so:
- `bldc_servo.cc` — the entire FOC/current-sense core — stays byte-for-byte stock;
- the logical-phase→winding mapping becomes identical to a stock r4.11, so calibration behaves identically;
- `motor.phase_invert` semantics survive (the alternative sense-side swap breaks silently if that flag is ever set — analyzed and rejected).

Exactly **one** side is swapped; swapping both would cancel back to broken. Status: implemented, builds green, image size unchanged. **Hardware validation at bring-up: first spin on a current-limited supply, verify calibration converges and phase currents track their windings.**

## Bus-voltage sense rescale (as-built R30 deviation)

v1.0 boards were assembled with **R30 (the VBAT_SENSE divider bottom leg) on the 1.2 kΩ BOM line instead of the intended 4.7 kΩ** — a late part-selection error (v1.0 consolidated all nominal-1 k resistors onto one 1.2 kΩ 1 % line to minimize BOM count, and R30 was swept in; value designer-confirmed) found in the pre-power-on design review. Left unaddressed, the bus would read ~4× low, every commanded phase voltage would be applied ~3.8× too large (calibration included), and the overvoltage/flux-brake protections could never trigger. **Fixed in software**: `vsense_adc_scale` in `fw/moteus_hw.cc` now encodes the as-built 100 k / 1.2 k ratio (`0.067943`; stock r4.11 = `0.017947`).

Consequences of the coarser scale (68 mV/LSB vs 18 mV/LSB): negligible for control — bus voltage is a filtered, slowly-varying scaling input, so the duty-conversion error is ~0.2 % worst-case; the dominant error is resistor tolerance (±1.3 %), which existed either way. **Mandatory bench check: compare `bus_V` telemetry against a DMM at first boot and trim the constant if they disagree.** At CATBOT's 8S operating point, also set `servo.max_voltage ≈ 38 V` so the flux brake engages near 35 V (the 46 V default puts it at 43 V — unreachable on 8S, so regen would otherwise pump the bus unclamped).

## Real-time constraints

The FOC current loop runs in a tens-of-kHz ISR (timer ISR samples currents; PendSV runs the math). Added features must never block it:
- **WS2812**: clock the 800 kHz stream via SPI-DMA or timer-DMA; never bit-bang in the ISR path.
- **I²C sensors (IMU/ToF)**: ride moteus's aux-port I²C engine (`fw/aux_port.h`) as device types, so one owner drives I²C1 and the later AS5600L joint encoder shares the same bus. The IMU is done this way (below); ToF follows the same pattern.
- **Servo**: timer PWM channel (50 Hz), trivial cost.
- **CAN handlers**: register reads copy pre-computed status — no work in the ISR.

## Flash budget (measured)

Built with the repo-pinned Bazel 7.4.1 (WSL Ubuntu-22.04), current FlexNode tree:

- Application image **437,232 B (427.0 KiB)** at `0x08010000` (+ 8.4 KiB CAN bootloader at `0x0800c000`, 472 B vectors at `0x08000000`).
- App window to the config region (`0x0807f000`) = 444 KiB → **~17 KiB free** for FlexNode's additions. ⚠️ Getting thin: the ToF summary registers and servo wrappers must be written lean; the IMU tilt demo (`led.imu_demo`, float HSV math) is a candidate to drop if space runs out.
- The additions fit that headroom if written lean and reusing existing moteus primitives (FDCAN, `fw/pid.h`, `fw/stm32_spi.h`, non-blocking I²C). Biggest consumer avoided by design: the VL53L7CX's ~84 KB init blob is **streamed from the host over the CAN diagnostic tunnel** instead of stored — see [can-layer.md](can-layer.md).
- ⚠️ 256 KB parts (`…CCU6`) **do not fit** (~2.2× over) — the MCU must be a 512 KB UFQFPN48 (`…CEU6`). LQFP48 parts are package-incompatible (no PC4/PC6).

## Status / lighting LED (WS2812B on PF0)

PF0 has no SPI-MOSI or usable timer alternate function on the G474 (its only timer AF is TIM1_CH3N, which is the motor PWM timer), so the WS2812 stream is bit-banged from the main loop in `fw/ws2812_led.cc` using the DWT cycle counter at the 170 MHz core clock. Interrupts are masked only for the *high* part of each bit (≤ ~0.85 µs), never across a frame, so the 30 kHz control ISR gains sub-microsecond jitter at most and can preempt during any low period (the WS2812B only resets after >50 µs low; the ISR is far shorter). Frames are sent only when the picture changes, plus a 1 Hz refresh, so static lighting costs nothing.

Pixel 0 is the onboard LED; pixels 1..`led.count` are external "master control" lighting on the same data line.

| config | default | meaning |
|---|---|---|
| `led.count` | 0 | external pixels after the onboard one (max 31) |
| `led.brightness` | 64 | global 0–255 scale |
| `led.master_r/g/b` | 0 / 80 / 255 | colour every pixel shows unless individually overridden |
| `led.fault_override` | 1 | pixel 0 blinks the fault code while `servo_stats.mode == 1 (fault)` |

**Pixel 0 states**, highest precedence first: fault → blink code; `led.imu_demo` → IMU tilt colour; otherwise **OK** = one blue fade-in (0.9 s) / fade-out (1.6 s) on entering OK, then dark. Re-armed whenever the board returns to OK (e.g. a fault clears). External pixels 1..N are unaffected and simply hold the master colour.

Fault code display: tens digit as amber blinks, gap, units digit as red blinks, long pause, repeat (a `0` units digit is one long red). Fault 35 (encoder) = 3 amber · 5 red. Telemetry group `led` reports frames sent, mode, pixel count, the fault code being shown and pixel 0's colour.

**Timing, and why it is the way it is (bring-up lesson, 2026-09-06).** Pulses are measured on the DWT cycle counter with a bounded spin. Two open-loop alternatives were tried on hardware and both failed in instructive ways: a counted `subs/bne` loop ran faster than assumed, shrinking the '0' pulse to ~230 ns, below the WS2812B's detection floor, so an all-'0' OFF frame registered nothing and the LED held its last colour indefinitely (coloured frames still rendered because their long '1' pulses carried the picture); unrolled NOPs ran slower than one cycle each under flash wait-state stalls, pushing '0' past the ~550 ns threshold, so '0's read as '1' and OFF decoded as turquoise. The cycle counter counts real core cycles and is immune to both. A static picture is sent exactly once (the WS2812B latches); there is no periodic refresh, which removed an occasional one-frame dimming caused by a resend colliding with an interrupt burst.

Live control today: `conf set led.master_r 255` etc. over the diagnostic channel (`moteus_tool --console`), `conf write` to persist. `Ws2812Led::SetPixel()` is the per-pixel hook for the CAN register block in [`can-layer.md`](can-layer.md).

## IMU (LSM6DS3TR-C on I²C1)

I²C1 on PB8/PB9 is, in moteus terms, the **aux2 port's I²C bus**, so the IMU is implemented as a new aux I²C device type rather than a second bus owner: `aux::I2C::DeviceConfig::kLsm6ds3` in `fw/aux_common.h`, driven by the existing non-blocking engine in `fw/aux_port.h`. On FlexNode it is **on by default**: `AuxPort` takes an `I2cDefault` (`kDefaultOnboardLsm6ds3` for aux2) that resolves `aux2.i2c.devices.0` from `kBoardDefault` to the IMU at address `0x6A`, 10 ms poll, and claims aux2 pins 0/1 as I²C. Set `aux2.i2c.devices.0.type 0` to disable it. Because PB8/PB9 are the sensor bus, aux2's stock debug-UART default is off on FlexNode.

- Init: one 3-byte burst write CTRL1_XL/CTRL2_G/CTRL3_C = `0x48 / 0x44 / 0x44` (104 Hz ODR, ±4 g, ±500 dps, BDU + auto-increment), then a WHO_AM_I read (expects `0x6A`), then 14-byte bursts from `OUT_TEMP_L` (temp, gyro XYZ, accel XYZ) every poll.
- Telemetry: `aux2.i2c.imu` — `active`, `whoami`, raw `ax ay az gx gy gz temp`, `nonce`, `error_count`. Raw LSBs; 0.122 mg/LSB, 17.5 mdps/LSB, 25 °C + temp/256.
- Registers: 0x098–0x09A accel (g: int8 0.1, int16 0.001, int32 1e-5), 0x09B–0x09D gyro (dps: int8 1, int16 0.1, int32 0.001), 0x09E temperature, 0x09F nonce (0 while inactive). Capabilities 0x080 bit2 follows `active`; status 0x081 bit2 = configured but not answering.
- Bench aid: `conf set led.imu_demo 1` paints pixel 0 from the IMU (hue = tilt direction, saturation = tilt, brightness rises with rotation rate; slow red blink = IMU silent). This is how the IMU was verified on 2026-09-06 with no CAN adapter on the bench.
- Not yet: re-init after a bus fault (a wedged device stays `active = false` with `error_count` climbing until reboot), configurable full-scale/ODR, orientation calibration.

## FlexNode register block (0x080–0x0FF)

Implemented in `moteus_controller.cc` as additional `Register` enum values plus `Read`/`Write` cases, per [`can-layer.md`](can-layer.md) §5. Live so far: 0x080 capabilities (bit2 IMU, bit4 pixel), 0x081 status, 0x098–0x09F IMU, 0x0B0–0x0B4 pixel mode/RGB/brightness (live, un-persisted; a `conf set led.*` clears them), 0x0FF block version = 1. Values above 127 need int16 or wider on the wire. **Unverified over CAN** — no adapter on the bench yet; first CAN session should read 0x080 and 0x0FF, then write 0x0B1 = 255 and watch the LED go red.

## Build & flash

```bash
# WSL (Ubuntu-22.04) — build from inside moteus-r4-parent/ (its WORKSPACE is the build root)
cd moteus-r4-parent
tools/bazel build --config=target //:target      # repo-pinned Bazel 7.4.1
# flash via SWD (FLASH header: NRST·CLK·DIO·3V3·GND) or, once running, the moteus CAN bootloader
# NRST: wire it while the ST-Link is powered by the laptop (it helps the debugger connect and
# does not hold the board). An UNPOWERED ST-Link left wired to NRST drags the line low and holds
# the board in reset, so for standalone running either unplug the ST-Link entirely or leave NRST off.
```

Windows/WSL notes (learned the hard way):
- From Windows, invoke as `wsl.exe bash -c '…'` — a login shell (`bash -lc`) breaks in this environment.
- **Line endings matter**: scripts executed in WSL (`tools/bazel`, `tools/workspace_status.sh`, `*.sh`) must be LF — a CRLF shebang yields `Unknown option: -` from python3. The repo `.gitattributes` pins `*.sh`, `*.py`, and `tools/bazel` to LF; don't fight it.
- When piping bazel output, check `${PIPESTATUS[0]}` (or the `INFO: Build completed` line) — `tail`'s exit code will happily lie to you.

## First-article checklist (before trusting the board)

1. ✅ Power-on smoke test: 5 V and 3.3 V rails, no heating, quiescent current sane. — *done 2026-09-06, 13 V bench adapter, both rails stable.*
2. 🟡 Flash over SWD; confirm boot, CAN enumeration, telemetry. — *SWD flash and boot done (ST-Link V2 + xPack OpenOCD; 512 KiB dual-bank G47x, IDCODE `0x20036469`); **CAN enumeration and telemetry still pending**.*
3. ⏳ **`bus_V` telemetry vs DMM** — must agree within ~0.5 V (validates the as-built R30 rescale; trim `vsense_adc_scale` if not).
4. ✅ `nBOOT0` option byte = boot-from-flash (PB8 doubles as BOOT0). — *done: `nSWBOOT0` cleared, so the I²C pull-up holding PB8 high no longer forces the bootloader.*
5. ⏳ Exercise the config-write path (`0x0807f000`) with a power-cycle; optionally confirm `DBANK = 1` via CubeProgrammer.
6. ⏳ Encoder bring-up on PC6 CS; verify AS5047 angle telemetry. — *encoder not yet soldered.*
7. ⏳ Bring-up config: `servo.max_current_A` ≈ 25–30 A (default 100 A is r4.11 legacy), `servo.vds_lvl_mv` toward 100–200 mV, `servo.max_voltage` ≈ 38 V for 8S.
8. ⏳ **Phase-order validation**: current-limited supply, `moteus_tool --calibrate`, confirm convergence and that commanded q-axis current produces torque without excess heating. Only then full current.

## Open firmware tasks

See [`roadmap.md`](roadmap.md); the CAN-layer additions (register block 0x080–0x0FF, IMU/ToF/pixel/load-cell drivers, AS5600L device type) are specified in [`can-layer.md`](can-layer.md).
