# Corenode — architecture decisions

_Opened 2026-09-08 03:10 IST, ahead of corenode schematic capture. Companion to
[`../docs/can-layer.md`](../docs/can-layer.md) and
[`../reports/2026-09-08-can-subsystem-design.md`](../reports/2026-09-08-can-subsystem-design.md)._

## The shape Aditya specified (2026-09-08)

> "I wanted the corenode's G4 to be on the canbus too, wherein the jetson talks to corenode
> interchangeably over canbus or uart… crucially, if the jetson hangs/crashes/is on low power mode,
> the stm32 can absolutely manage the cat's basic locomotion systems (run basic inverse kinematics
> and read onboard imu as a fallback just in case), just like we know if we're falling even if
> we're asleep."

Three tiers, each able to survive the one above it dying:

| Tier | Runs | Survives |
|---|---|---|
| Jetson | neural policy, VSLAM, planning | allowed to crash |
| **Corenode (G4)** | CAN bus master, basic IK, attitude fallback, safe-state authority | must not crash |
| FlexNode ×10 | FOC at 30 kHz, on-node effector loops, G0 timeout | corenode loss |

## Decision: two FDCAN peripherals, not one shared bus

The G4 has **3× FDCAN**. Use two:

- **FDCAN1 → the FlexNode chain** (10 nodes, 45 cm, 1 M/5 M, terminated at both ends).
- **FDCAN2 → the Jetson link** (or UART — chosen later for convenience/reliability).

> This is the important schematic-capture consequence. Putting the Jetson on the *same* bus as the
> fleet would (a) break the single-master invariant the whole schedule rests on, and (b) let Jetson
> traffic arbitrate against the 350 Hz gait loop. Two independent peripherals cost two transceivers
> and buy total isolation: **Jetson traffic can never perturb the fleet loop, by construction.**
> If the Jetson link is UART instead, the same isolation holds for free — but wire FDCAN2 anyway so
> the choice stays open, since that is exactly the interchangeability Aditya asked for.

If the Jetson does sit on FDCAN2, it is **a client of the corenode, not a bus master** — it
addresses the corenode only, never a FlexNode directly.

## Decision: the corenode needs its own IMU

The fallback ("know we're falling even while asleep") must not depend on any FlexNode being alive,
and must not cost a CAN round trip. So the corenode carries **its own IMU on its own I²C**, not a
FlexNode's read over the bus.

This is a **schematic requirement, not a firmware choice** — get it on the board now. An
LSM6DS3TR-C keeps the part count and the driver shared with FlexNode.

## The latency question, answered with numbers

Aditya: *"the imu reading is going thru the stm32… won't that introduce latency issues especially
since there is a neural policy running, or is that orders of magnitude slower, since I recall the
policy to be fine if ran at 60-100fps?"*

**The instinct is right about the link and wrong about where the latency lives.** Against a 100 Hz
policy tick (10 ms budget):

| Term | Cost | Share of budget |
|---|---|---|
| **IMU sample age at 104 Hz ODR** | **0–9.6 ms** (mean 4.8) | **up to 96 %** |
| I²C burst read, 12 B @ 400 kHz | ~300 µs | 3 % |
| I²C burst read, 12 B @ 1 MHz (Fm+) | ~120 µs | 1 % |
| CAN-FD hop, if read via a FlexNode | ~100 µs + poll phase | 1 % |
| UART to Jetson, 32 B @ 921600 | ~350 µs | 3.5 % |
| **All transport combined** | **< 1 ms** | **< 10 %** |

So the STM32 in the path costs well under a millisecond — two orders of magnitude of headroom,
exactly as expected. **The problem is the sensor's own output rate.**

> Gotcha, 2026-09-08 03:10 IST: the LSM6DS3TR-C is configured at **104 Hz**
> (`fw/aux_port.h:667`, `LSM6DS3_CTRL1_XL_104HZ_4G = 0x48`). At 104 Hz a sample can be **9.6 ms old
> before it is even read** — essentially the entire budget of a 100 Hz policy, and beating against
> the policy tick so the age varies randomly between 0 and 9.6 ms sample to sample.
>
> **Jitter is worse than mean latency for a policy trained in sim.** A policy that sees state of
> randomly varying age is being fed a different system than it was trained on.
>
> Fix: raise ODR. The part goes to 6.66 kHz. **416 Hz** (`CTRL1_XL = 0x68` at ±4 g) gives 0–2.4 ms;
> **833 Hz** (`0x78`) gives 0–1.2 ms. Cost is I²C bandwidth, which §I²C-loading in
> [`hardware-io.md`](hardware-io.md) says we have — especially at Fm+. Then **phase-lock the read to
> the control cycle** so the age is deterministic rather than merely small.

## Basic IK on the corenode — feasible

Closed-form 3-DOF leg IK is a few hundred flops per leg. The G4 is 170 MHz with a hardware FPU;
4 legs at 350 Hz is trivially inside budget. The corenode's realtime job is the bus schedule, and
IK is small beside it. **No concern.**

## Open snags before schematic capture

1. **Corenode MCU must be FDCAN-capable.** A G0 has no FDCAN at all — settled in
   [`open-questions.md`](open-questions.md) §5. A G4 gives 3× and the two-bus split above.
2. **Jetson Orin Nano CAN availability.** The SoC has CAN controllers, but whether they are broken
   out depends on the carrier board, and a transceiver is needed either way. **Check the carrier
   before committing FDCAN2 to a Jetson-side CAN link** — if it is awkward, UART is the fallback and
   the isolation argument is unaffected.
3. **Termination.** The corenode sits at one physical end of the chain, so it needs a 120 Ω
   terminator — same solder-bridge arrangement as FlexNode. Only the two ends.
4. **Bit timing must be identical** on the corenode and every FlexNode, including the TDC change in
   [`open-questions.md`](open-questions.md) §9. Do not let the corenode be programmed with stock
   moteus timing while the nodes carry the corrected values.
5. **Fallback authority needs defining**: what "basic locomotion" the corenode is allowed to command
   when the Jetson is gone, and how it hands back. Firmware question, not schematic — but decide the
   scope before writing it, and keep it in the reducing-actions spirit of `can-layer.md` §7.
