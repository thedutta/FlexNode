# WS2812 status LED

Driver: `moteus-r4-parent/fw/ws2812_led.{h,cc}`, one WS2812B on **PF0**, constructed inside `MoteusController::Impl`, polled at 1 kHz. Config group `led`, telemetry group `led`.

## Behaviour (as of `ef0d948`)
Pixel 0 is the node's status LED. Precedence, highest first:
1. **Fault** (`servo_stats.mode == 1` and `led.fault_override`): blink the fault code, tens digit amber, gap, units red, long pause, repeat. `x0` = one long red. Fault 35 (encoder) = 3 amber · 5 red.
2. **`led.imu_demo`** (bench aid, default 0): hue = tilt direction, saturation = tilt, brightness rises with rotation rate; slow red blink = IMU silent. How the IMU was verified with no CAN adapter.
3. **OK**: one blue fade-in (0.9 s) / fade-out (1.6 s) on entering OK, then **dark**. Re-armed when the board returns to OK (a fault clearing). Colour = `led.master_*` (default 0/80/255 at brightness 64).

Pixels 1..`led.count` are external "master control" lighting: hold the master colour, unaffected by pixel-0 status. `Ws2812Led::SetPixel()` is the per-pixel hook for the CAN registers. Registers 0x0B0–0x0B4 set mode/RGB/brightness live and un-persisted; a `conf set led.*` clears them.

A static picture is transmitted **once**. No periodic refresh: the WS2812B latches, and a resend that landed during an interrupt burst produced a visible one-frame dimming about once a minute.

## Timing: DWT only. This is the rule.
Pulses are timed on `DWT->CYCCNT` (real core cycles), interrupts masked only during each bit's high pulse, low period left interruptible (the part tolerates a long low; it only latches after >50 µs). Bounded spin (`kSpinGuard`) so a stopped counter can produce a wrong pulse but never a hung loop; the constructor verifies the counter advances and `Transmit()` is skipped if it doesn't.

Two open-loop replacements were tried on hardware on 2026-09-06 and **both failed**:

| Method | Real T0H | What the eye saw | Why |
|---|---|---|---|
| counted `subs/bne` loop, assumed 4 cyc/iter | ~230 ns (too short) | colours fine; **off frame ignored, LED holds its last colour forever** | an all-`0` frame has no pulse the part can detect; coloured frames survive on their long `1` pulses |
| unrolled NOPs, assumed 1 cyc each | >550 ns (too long, flash stalls) | blue-green wobble during the fade; off frame decodes **turquoise** | `0`s read as `1` |
| DWT cycle counter | 350 ns exactly | clean fade to dark | counts real cycles, immune to stalls |

> Gotcha: **a colour that never turns off is a `0`-bit timing bug**, not a logic bug and not a stale image. Coloured frames can look perfect while off frames silently fail. Spent hours on wrong theories (reset not taking, DWT frozen on cold boot) before a symbol read showed `want=0, frames settled, LED lit`.

PF0 has no SPI-MOSI and no usable timer alternate function on the G474 (only TIM1_CH3N, the motor timer), so bit-banging is the only option without a hardware change. Frame cost: 24 bits × ~1.3 µs per pixel; 32-pixel cap → ~1 ms worst case, and only on change.

## Verifying a change to this driver
1. Flash, then **power-cycle** and watch the first 3 s. The breath is the only visible event.
2. If it "looks steady", that is the trap above. Read the internals before theorising.
3. `led.imu_demo 1` on a bench build is the fastest way to prove polling + transmit end to end.

<!-- self: default breath timings kOkFadeInMs=900 / kOkFadeOutMs=1600 are in ws2812_led.cc. He may later want a repeating breathe; that's a small change in OkEnvelope (wrap instead of return 0). -->
