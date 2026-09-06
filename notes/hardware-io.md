# Hardware I/O — what the board actually has

What a FlexNode v1.0 can physically be asked to do, and what it cannot. Written because the CAN
channel model made the limits concrete, and because two of them were nearly designed around
incorrectly. Companion to [`../docs/hardware.md`](../docs/hardware.md) (the polished version) and
[`../docs/can-layer.md`](../docs/can-layer.md) §9.

## Off-board I/O inventory

_Recorded 2026-09-07 01:35 IST, from `fw/moteus_controller.cc:401-500` (family-0 aux tables) and
`fw/moteus_hw.cc` (pin remaps), cross-checked against `docs/hardware.md`._

| Resource | Pins | Peripheral backing | Notes |
|---|---|---|---|
| I²C1 | PB8 / PB9 | `I2C1`, `USART3` alt | aux2. Onboard LSM6DS3TR-C (0x6A) + VL53L7CX (DNP) + J2 external port. 2 kΩ pull-ups |
| Servo / LED out | PC13 | **TIM1_CH1N (AF4), TIM8_CH4N (AF6)** | aux1 pin 0. No *aux-table* timer, but the silicon has two |
| SPI2 pads | PB13 / PB14 / PB15 | `SPI2`, ADC | aux1 pins 1–3. Also ADC-capable |
| 5 V rail sense | PB11 | ADC | 50 mΩ high-side shunt, ~32 mA/LSB |
| ToF INT | PB10 | EXTI | |
| Debug / GPIO | PC14 / PC15 | nothing | aux2 pins 2–3 |
| WS2812 | PF0 | bit-banged, DWT-timed | status LED, see [`ws2812-led.md`](ws2812-led.md) |
| Rotor encoder | PC6 (CS) | SPI1 | AS5047P, **not yet soldered** |

> Gotcha, 2026-09-07 02:40 IST: **"PC13 has no timer" is false as a hardware claim** — I recorded
> it that way earlier. It has no timer *in the moteus aux table*. The G474 maps `PC_13` to
> **TIM1_CH1N (AF4)** and **TIM8_CH4N (AF6)** (`TARGET_STM32G474xE/.../PeripheralPins.c`), so a
> **hardware-timed servo pulse via TIM8_CH4N** is available — and TIM8_CH4N is the right choice
> over TIM1_CH1N because TIM1 is wanted for the SimpleFOC Mini below. The RTC-domain drive
> caveats (weak ~3 mA, slow slew) still stand. Lesson: an absent entry in a vendor table is not an
> absent capability in the silicon.

> Gotcha, 2026-09-07 01:35 IST: **the aux hardware tables are still stock moteus r4 pinouts.**
> `GetAux1HardwareConfig()` / `GetAux2HardwareConfig()` in `fw/moteus_controller.cc` were never
> updated for FlexNode, because FlexNode reports `hw_family = 0` and inherits the r4 tables.
> Aux2 happens to be right (PB8/PB9 I²C is genuinely where the IMU is, and it is verified working).
> Aux1 is **not** verified against the FlexNode PCB — PC13 matches, PB13/14/15 are plausible given
> the "SPI expansion pads" in `docs/hardware.md`, but nobody has traced them on a real board.
> Do that before designing anything that depends on aux1.

## Per-node peripheral assignment

_From Aditya, 2026-09-07 01:35 IST. This is design intent, not something read off the board._

Every node is "one actuator plus what that joint needs to know about itself", and the assignment
was **spaced deliberately from the beginning** so no node is overloaded:

- **One servo per node, never two.** The 7 DS3235/DS3230 servos hang off 7 *different* FlexNodes.
- **Shoulder nodes (2× GIM 8108-8) each carry a SimpleFOC Mini as well as their main axis.** One of
  the two also has a DS3235; the other has spare capacity for load cell / RGB / whatever.
- Nodes are meant to have a **local id/name**, not just a numeric CAN id — see
  [`can-layer-design.md`](can-layer-design.md).

> Gotcha, 2026-09-07 01:35 IST: I initially recommended a **PCA9685 I²C PWM expander** to solve
> "seven servos, one pulse pin". **That recommendation was wrong and is withdrawn** — it solved a
> problem that does not exist, because the servos were never going to share a node. One pulse on
> PC13 is sufficient *per node* — and per the correction above it can even be hardware-timed via
> TIM8_CH4N. The lesson is to ask how the mechanical layout distributes load before optimising a
> per-node resource.

## The trades available

Aditya, 2026-09-07 01:35 IST: *"pc13 is definitely not alone, the i2c pins can be repurposed as
gpio if the lsm6dtr is not used — didnt think that one out i think desoldering it works though.
i2c fallback gpio expansion is always an option."*

So the per-node resource picture is a **trade**, not a fixed budget:

| Keep | Give up | Gain |
|---|---|---|
| IMU + ToF + I²C encoders + I²C ADC on PB8/PB9 | PB8/PB9 as GPIO | the whole I²C peripheral ecosystem on 2 wires |
| PB8/PB9 as plain GPIO (desolder the LSM6DS3TR-C) | IMU, ToF, all I²C peripherals | 2 free GPIOs |
| I²C **plus** an expander (PCA9685 / MCP23017) | nothing | many PWM or GPIO channels on the existing 2 wires |

The expander option is the interesting one and stays on the table for *future* nodes that need
more channels than v1.0 breaks out — it just isn't needed for the servos.

Practical consequence for the channel model: a node's populated channel list is genuinely
per-node, and two nodes with identical silicon can differ by a desoldering operation. That is
exactly why capabilities are discovered at runtime (register 0x080 / channel `type`) rather than
assumed from a part number.

## What is still genuinely blocked

**The SimpleFOC Mini.** A Mini (DRV8313-class) needs 3 PWM + enable. PC13 cannot supply that, and
the shoulder nodes need it *simultaneously with* their own GIM 8108-8 on the main axis — so this
is not an either/or with the onboard FOC, it is an addition to it.

Candidate path: **PB13/PB14/PB15 → TIM1_CH1N (AF6) / CH2N (AF6) / CH3N (AF4)**, confirmed in
`TARGET_STM32G474xE/.../PeripheralPins.c`. Three hardware PWM channels from one timer is exactly
what a Mini wants.

> Gotcha, 2026-09-07 02:40 IST: **motor PWM is on TIM5, not TIM2.** I recorded TIM2 earlier and it
> was wrong. `moteus_hw.h` defaults `pwm1/2/3 = PA_0_ALT0 / PA_1_ALT0 / PA_2_ALT0`; on the G474
> map `PA_x_ALT0` is **TIM5_CHx (AF2)** — plain `PA_x` would have been TIM2 (AF1). TIM5 is also the
> CAN bootloader's time base. The conclusion (TIM1 is free) survives; the reasoning did not.

**Timer allocation, audited 2026-09-07 02:40 IST.** Claimed: **TIM5** (motor PWM + bootloader),
**TIM15** (mbed `us_ticker` + `MillisecondTimer`), **LPTIM1** (ADC trigger, `bldc_servo.cc:685`);
TIM2/TIM3 only if aux1 hardware quadrature is configured; TIM4 only in non-FlexNode families.
**Free: TIM1, TIM8, TIM16, TIM17, TIM20.**

Four constraints on the TIM1 path, none fatal, all easy to trip over:
- **`PB_14` is in FlexNode's aux1 pin table** (slot 1, ADC ch5) — must be `kNC` or aux and the Mini
  fight over it.
- **CHxN-only drive needs `CCxNE`/`CCxNP` + `MOE`** — complementary outputs are not enabled by the
  ordinary channel-enable path.
- **TIM1 BRK shares a vector with TIM15** (`TIM1_BRK_TIM15_IRQn`), which mbed uses. Don't enable
  TIM1 break interrupts.
- **A shoulder node with both a Mini and a DS3235 cannot put the servo on TIM1_CH1N/PC13** — same
  channel as PB13. Use **TIM8_CH4N** for the servo.

Costs:
- the SPI expansion pads (SPI peripherals and an external BLDC become mutually exclusive);
- a **second commutation path in firmware** — open-loop or lightly closed against an I²C
  AS5600/MT6701, not a second FOC stack. Budgeted **~10–13 kB**, which now fits easily. **Write it
  separate; share nothing with `bldc_servo`.**
- **CPU**, on a core already running a 30 kHz FOC ISR. **Headroom unmeasured** — no ISR cycle
  instrumentation exists. Only metric: `system_info.idle_rate` (`system_info.cc:75`), readable over
  SWD as a delta of `moteus::SystemInfo::idle_count`.

> Gotcha, 2026-09-07 02:40 IST: **the CORDIC is a single shared peripheral**, used inside the FOC
> ISR with a write→read sequence. A main-loop user gets pre-empted mid-transaction and reads
> someone else's result. A Mini commutation path must use **software sin/cos or a LUT — never the
> shared CORDIC.** Failure mode would be rare unreproducible torque glitches on *both* axes, which
> is close to the worst thing to debug on this machine.

Firmware-side availability confirmed 2026-09-07 02:40 IST; **pad breakout on the real board is not** — trace it. Three options and their trade-offs are written up
in [`../docs/can-layer.md`](../docs/can-layer.md) §9.2. Nothing should be committed to a v1.1
board revision until TIM1 availability and the PB13/14/15 breakout are confirmed on real hardware.

## I²C bus loading

Everything shares one bus: IMU + ToF + every I²C encoder + a load-cell ADC + any expander. At
400 kHz an AS5600 angle read is ~150 µs and an IMU burst ~300 µs, so a node polling both at 1 kHz
spends ~45 % of the bus. Two cheap mitigations:

- The 2 kΩ pull-ups are **already sized for Fast-mode Plus (1 MHz)**, which cuts all of the above
  by ~2.5×. Verify with a scope on the first assembled node.
- Poll rates need not match the control rate. An RC servo's outer loop at 200 Hz is plenty, and
  the IMU is configured for 104 Hz anyway.

Part choice matters here: prefer **NAU7802** (I²C, 24-bit) for load cells over HX711 (2-wire
bit-bang, needs two GPIOs) and ADS1220 (SPI, contends with the pads above).
