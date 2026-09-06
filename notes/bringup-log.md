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

## 2026-09-07 02:20 IST — open-loop motor test image prepared (NOT YET RUN)

First-energisation bench image built on branch `bench/openloop-test`, commit `843566b`. **Not on
main, not pushed, and not yet flashed or run.** The power stage has still never been energised.

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

Still unvalidated: `bus_V` has never been checked against a DMM (a refusal with bus-range reason
would be the first sign the R30 rescale is wrong); the DRV8353 has never been enabled and its
gate-drive currents are inherited r4.11 values for different FETs; reading RAM without halting via
this ST-Link clone is untested.
