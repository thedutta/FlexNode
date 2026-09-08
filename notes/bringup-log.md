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

(The "still gated" table that stood here as of 2026-09-06 is superseded by the one at the end of the file.)

## 2026-09-07 02:20 IST — open-loop motor test image prepared

First-energisation bench image built on branch `bench/openloop-test`, commit `843566b`. Not on
main. At the time of writing the power stage had never been energised; the runs are in the next entry.

Motor GIM 8108-8, no encoder (AS5047 unsoldered, magnets not arrived), 13 V 1 A current-limited
supply. Uses stock `kVoltageFoc` (mode 7) — commanded electrical angle + voltage, sinusoidal, **no
encoder, no `theta_valid` check** — commanded through the same `BldcServo::Command()` entry the CAN
register writes use. **FOC / PWM / modulation / current-sense / gate-driver code untouched.**

Safe voltage, computed not guessed: R phase-to-phase 0.439 Ohm -> 0.2195 line-to-neutral; the 1 A
supply is *not* the binding constraint (it would allow ~1.38 V / 6.3 A), phase current is. Chosen
`kVoltageRunV = 0.30 V` -> ~1.4 A peak phase, ~0.6 W, ~50 mA from the bus (5% of the limit), duty
2.3%. Ramped from 0 at 0.5 V/s, never stepped, hard-capped at 0.5 V. Expected motion is **2.9-4.5
degrees at the output — mark the rotor bell or you will not see it. A twitch is success.**

Guards: 15 s armed-after-delay with LED countdown; pre-arm refuses unless bus 11.5-14.5 V, FET
5-45 C, servo stopped and fault-free; aborts latch de-energised on FET >55 C, |I| >4 A, bus <11 V
or >15.5 V, any moteus fault, or mode-timeout 500 ms. Budget: 5 loops / 34 s cumulative energised /
20 min uptime, whichever first. Config defaults tightened by the build define: `max_current_A 5`,
`max_power_W 20`, `max_voltage 18`, `fault_temperature 60`, `timeout_mode 0`, `drv8323.vds_lvl_mv
100` (~62 A trip; the inherited 700 mV was ~440 A, i.e. no protection at all).

Image 444,504 B, 10,152 B free. Telemetry struct `g_bench_telemetry` at `0x20000d68`, 26 words,
word 0 must read `b3ac0001`.

> Gotcha, 2026-09-07 02:20 IST: `max_current_A` / `max_power_W` are enforced by the **current
> controller**, which kVoltageFoc does not run. In this mode the only real protections are the
> 1 kHz software checks, the DRV VDS OCP, and the bench supply limit. Do not read those config
> values as safety.

> Gotcha, 2026-09-07 02:20 IST: **never `halt` while energised** — a halt freezes the PWM
> mid-vector with one phase pair held on. The `bench` telemetry mode reads RAM without halting;
> `benchhalt` and `diag` are only safe while the LED shows rest / done / fault.

> Gotcha, 2026-09-07 02:20 IST: this test does **not** validate the phase-order fix. Open-loop
> voltage FOC is indifferent to sense pairing, so "forward" is an arbitrary direction. It validates
> that the stage switches and produces torque, nothing more. The phase-order gate stays closed.

Known-good image preserved at `tools/bench/out-good/` (md5-verified against the ef0d948 build) so
the working firmware can always be restored.

Unvalidated at the time: `bus_V` against a DMM, the DRV8353 enable, and reading RAM without halting
via this ST-Link clone. All three were closed on 2026-09-07/08 — next entry.

## 2026-09-07 → 2026-09-08 — first spin (branch `bench/openloop-test`, `843566b` → `0e29bad`)

Recorded 2026-09-08 13:20 IST from the bench-branch commit messages and the STAGE CONSTANTS block of `flexnode_bench_openloop.cc`; the test images stay on the branch and `main` is unchanged. Motor GIM 8108-8 (8:1 output), no encoder, 13 V adapter whose current limit measured at 1.03 A. Open-loop `kVoltageFoc` through the stock `BldcServo::Command()` path — FOC / PWM / current-sense / gate-driver code untouched.

- First run (2026-09-07) aborted at the end of the countdown on a **false** overcurrent: the DRV8353 CSA outputs sit at 0 V while the driver sleeps and moteus calibrates offsets only after enable. Fix `8f28710`: current checks gated on `servo_mode == kVoltageFoc`, new `kSensorInvalid` class. Bus 12.4–12.5 V and FET 31.9 °C read correctly from boot.
- **DRV8353S enabled; the power stage switched; the motor turned.** Run 2 (0.30 V, 0.1 rev/s) moved the output 103/104° per movement as designed, 1,047 mA measured vs ~1.0 A modelled after the dead-time loss.
- **Phase-current sense validated**: all three offsets calibrate once the driver is enabled; phase currents sum to ~0 (the `kSensorInvalid` check depends on it). V–I points fit R ≈ 0.364 Ω/phase, V_dt ≈ 30 mV (honest range 0.33–0.44 Ω; the stage-1/2 predictions had halved the datasheet R — `40a25a6`).
- **Stage 2** (1.33 V / 0.8 rev/s, `9ef8d20`): **8 loops, 140 s energised, zero faults, FET plateau 38.7 °C.** Forward half-sine 4.5 s, reverse square 2.875 s.
- **Stage 3** (3.325 V / 2.0 rev/s, `40a25a6`) sag-aborted at the 1.8 V sweep step exactly as modelled (4.83 A, 12.9 W, 1.03 A; adapter collapsed 12.57 → 10.46 V). **The bus-sag abort works and de-energises cleanly.** Stage 3 waits for a bigger supply; stage 2R (`0e29bad`) rolls the drive back to stage-2 parameters.
- **`bus_V` vs DMM done** (over SWD telemetry, not CAN): DMM 12.82 V vs board 12.50 V (12.64 V running max) → ratio 1.0145–1.0255, midpoint applied: `vsense_adc_scale` 0.067944 → 0.069540 (bench branch only so far). The R30 rescale was right to ~2.5 %.
- V–I sweep segment (`7cc9617`, `0e29bad`): five aligned voltage steps per loop, least-squares R and V_dt with residuals, ΔR/R → winding ΔT at 0.393 %/K. Sensorless motor temperature, which the unpopulated NTC pad cannot provide.

Bench image 445,936 B; 8,720 B free.

**Not validated by any of this:** the phase-order fix (open-loop drive is indifferent to it — gate stays closed); anything closed-loop (AS5047 still unsoldered: no calibration, no position control has ever run); CAN (no adapter; the register block has never been read); servo output, 5 V rail sense, ToF; switching edges / EMI on the BSC016N06NS (not scoped).

## Still gated (as of 2026-09-08)
| Gate | Status | What unblocks it |
|---|---|---|
| CAN enumeration, telemetry, register block over the wire | not started | a CAN-FD adapter (fdcanusb or CANable-FD) |
| Encoder bring-up on PC6 | not started | AS5047 soldered to the back side |
| Config-write path (0x0807F000) survives power cycle | not started | `conf write` over CAN, then power cycle |
| **Phase-order validation** | not started — open-loop spin cannot test it | encoder + current-limited `moteus_tool --calibrate`, watch convergence and phase currents. **Hard gate before closed-loop drive.** |
| `vsense_adc_scale` trim onto `main` | on the bench branch only | port the one-line change from `0e29bad` |
| Bring-up config: `servo.max_current_A` 25–30, `servo.vds_lvl_mv` 100–200, `servo.max_voltage` ≈ 38 (8S) | not applied | CAN |
| Gate-drive edges / EMI for BSC016N06NS (inherited r4.11 currents) | drove ≤ 4.8 A without incident; edges not scoped | a scope on the first closed-loop spin |

