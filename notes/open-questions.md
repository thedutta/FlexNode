# Open questions

Decisions not yet made, with the current recommendation. Edit in place when one closes.

## 1. Flash budget — MEASURED 2026-09-07, largely solved
~16.8 kB free at bee0685. **See docs/flash-budget.md.** The old recommendation in this file was
wrong about where the space is, and is superseded.

> Gotcha: the assumption was "reclaim the unused encoder drivers first". Measured, they are only
> ~13.4 kB and several already measure **zero** — `--gc-sections` had removed them years ago.
> The real lever is mjlib serialisation: **79,518 B (18% of the image)** of SerializableHandler<T>
> template instantiation, ~32 kB of it droppable by deleting telemetry registrations. The tell was
> ws2812_led.o at 13.8 kB for a driver whose actual logic is 3.9 kB — the other 8.4 kB was schema
> and text-parsing code for its Config and Status structs. Every Register() call costs 2-8 kB.

Order: (1) drop 7 telemetry registrations ~32 kB, (2) re-measure, (3) nano.specs ~22 kB exposure,
(4) -Os on moteus_controller.o, (5) encoder drivers / BoardDebug, (6) motor-less profiles LAST
(touches the motor path; probably never needed).

Consequence: profiles are no longer load-bearing for space. One image fits the whole v2 CAN layer.

## 2. SimpleFOC driver topology — ANSWERED (2026-09-07), but blocked on hardware
Aditya: the SimpleFOC driver is a **peripheral of a FlexNode**, with its own AS5600 for feedback
— not its own CAN node. So no classic-CAN coexistence problem.

> Gotcha: v1.0 silicon probably cannot honour that. A Mini needs 3 PWM + enable; FlexNode v1.0's
> only aux output is PC13, which has **no timer** (family-0 aux table, fw/moteus_controller.cc:406).
> Candidate path is TIM1_CH1N/2N/3N on the SPI2 pads (PB13/14/15) — TIM1 looks free because motor
> PWM is on TIM2 (PA0/1/2). Unverified. Costs the SPI pads and needs a second commutation loop.
> See docs/can-layer.md §9.2 for the three options; option 1 (head driver gets its own MCU and
> becomes a CAN peer) is currently preferred.

## 2b. Servo count — one output, seven servos needed
Same root cause. CATBOT has 7 DS3235/DS3230 servos; v1.0 has one software-timed pulse pin.
**Recommendation: PCA9685 on the existing J2 I2C port.** 16 hardware PWM channels over wires that
already exist, no respin, and PB11 still current-senses the whole 5V rail for stall detection.
Needs a decision before the aux-node connector work. docs/can-layer.md §9.1.

## 2c. Load-cell ADC part
NAU7802 (I2C, 24-bit) recommended over HX711 (2-wire bit-bang — needs two GPIOs v1.0 hasn't got
to spare) and ADS1220 (SPI — contends with the pads in 2 above). Keeps everything on one bus.

## 3. CAN adapter — possible workaround found 2026-09-07 03:05 IST
None on the bench. Everything after the LED and IMU is blind without one.

**Possible unblock:** moteus's `kSerial` aux UART mode speaks the full register protocol over a
3.3 V UART, and family-0 USART3 sits on PB8/PB9 — FlexNode's J2 I2C header. A ~$2 USB-TTL adapter
may therefore give `moteus_tool` access with no fdcanusb. Costs the IMU while active (same pins).
See notes/hardware-io.md. **Untested.** If it works it is the cheapest possible unblock and should
happen before anything else. First CAN session: `moteus_tool --info`, read 0x080 and 0x0FF, `bus_V` vs DMM, write 0x0B1=255 and watch the LED go red.

## 4. Encoder
AS5047 not soldered. Until it is: no calibration, no position. Encoder bring-up on PC6 is its own gate.

## 5. IMU details deferred
Full-scale and ODR are fixed in firmware (±4 g, ±500 dps, 104 Hz). No re-init after a bus fault. No orientation calibration. Fine for now; revisit when fusion starts.

## 6. LED extras deferred
Breathe/chase modes (0x0B0 = 2/3) are reserved and currently treated as solid. Per-pixel CAN writes exist as an API (`SetPixel`) but have no register yet.

## 7. Portfolio / site
Bench photos: one first-light photo is live. More can go in as milestones land. The subagent handles it; give it facts, never let it invent images or numbers.

## 8. Fleet questions raised by the v2 CAN design (2026-09-07)
- **10 vs 13 nodes.** Published actuator list is 4 GIM8108-8 hips + 6 5010 knees = 10 BLDC (one
  FlexNode each), plus 2 head gimbals on a SimpleFOC dual driver and 7 servos on aux ports. The
  "thirteen nodes" figure is unreconciled. Assumed 10 actuator + 3 non-actuator. Changes the
  profile mix, not the protocol.
- **Brainstem MCU: G0 or G4?** The site says STM32G0 in one place ("zero-watt acoustic wake") and
  STM32G4 in another ("spinal cord driving two chains"). **G0 has no FDCAN.** If the brainstem is
  to be the Phase-B realtime bus master it must be a G4 (3x FDCAN). This decides whether the
  two-chain topology — the thing that buys 400 Hz+ — is reachable at all. Settle early.
- **Is the Jetson on the CAN bus in production, or only behind the brainstem?** Determines whether
  there are two masters and where the G1 presence beacon originates.
- **G2 deputy is the most dangerous idea in the design.** A node that can command its peers can do
  so while wrong. Whitelist (reducing actions only), authority expiry, and default-off rank are
  what make it survivable. Do not relax any of the three for convenience. docs/can-layer.md §7.
