# FlexNode — CAN Layer Architecture

_v2, 2026-09-07. Supersedes the v1 design of 2026-07-21 (the v1 register block 0x080–0x0FF is retained verbatim — see [§4](#4-the-node-block-0x0800x0ff-v1-retained)). Companion to [firmware.md](firmware.md), [hardware.md](hardware.md), [roadmap.md](roadmap.md)._

**Status:** design. Implemented today: node block 0x080/0x081, IMU 0x098–0x09F, pixel 0x0B0–0x0B4, version 0x0FF. Everything in [§3](#3-the-channel-model) and [§7](#7-host-loss-and-inter-node-autonomy) is unbuilt. **Nothing in this document has been exercised over a real CAN bus** — there is no adapter on the bench yet.

---

## 0. What this layer is for

FlexNode is a hardware fork of moteus with one purpose: make CATBOT buildable by making a *node* the unit of composition instead of a *motor controller*. A node is "one actuator plus whatever that joint needs to know about itself". The CAN layer is the enabler — it is what turns thirteen boards into one machine, and it is also the product's public API if FlexNode is ever sold.

Two properties follow from that, and they drive every decision below:

1. **The register map is a stable, versioned, public interface.** It must not change meaning between boards, builds, or firmware revisions. A host written against FlexNode block v2 must work against every node claiming v2, forever.
2. **What a node *is* may vary; what a register *means* may not.** Nodes differ in populated hardware and compiled features. They never differ in semantics.

### What changed since v1

| | v1 (2026-07-21) | v2 (this document) | Why |
|---|---|---|---|
| Configuration | One image, thirteen configs | One image *per profile*, config within a profile | Aditya, 2026-09-07: hardware changes can't happen at runtime, so a reflash on hardware change is "fully acceptable". This is the release valve for the flash budget. |
| Peripheral model | Fixed registers per peripheral kind | **Typed channel slots**, strided registers | Fixed registers don't survive "any accommodatable combination". Slots do, and they make the map sellable. |
| SimpleFOC driver | Open question: peripheral or its own node? | **Peripheral of a node** | Answered 2026-09-07. See the hardware gap in [§9](#9-hardware-gaps-that-this-design-exposes) — v1.0 silicon may not be able to honour it. |
| Host failure | Not addressed | [§7](#7-host-loss-and-inter-node-autonomy), three graded levels | "potentially have inter-flexnode comms too, if jetson goes dead and hangs" |
| Unsolicited transmit | Never | Never, **except** a config-gated deputy in G2 | Scoped exception, not an abandoned invariant |

### Scope note on node count

The published CATBOT actuator list is 4× GIM8108-8 hips + 6× 5010 knees (BLDC, one FlexNode each) + 2 head gimbals on a SimpleFOC dual driver + 7 DS3235/DS3230 servos on node aux ports = 19 DoF but only **10 BLDC actuators**. The "thirteen nodes" figure elsewhere is therefore 10 actuator nodes + ~3 non-actuator nodes (servo/sensor duty). This document assumes 10 + 3 and sizes the bus for 13. **Flagged as unreconciled** — the split changes the profile mix in [§6](#6-profiles-build-time-roles), not the protocol.

---

## 1. Layering

Keeping these four layers separate is what lets the top two change without breaking moteus tooling.

| Layer | What it is | Ours? |
|---|---|---|
| **L0 Transport** | CAN-FD, 1 Mbit arbitration / 5 Mbit data. Frame id = `(source << 8) \| destination`; high bit of source = reply requested. `can.prefix` occupies the upper extended-id bits and namespaces a bus. **Destination `0x7f` is a broadcast every node accepts** (verified in `fw/moteus.cc:307` — filters 1 and 3 accept `prefix<<16 \| 0x7f` for both standard and extended frames). | stock |
| **L1 Register protocol** | moteus multiplex subframes: read/write N *consecutive* registers at int8/int16/int32/float resolution. Register numbers are varuints, so anything ≤ 0x3FFF costs 2 bytes. | stock, untouched |
| **L2 Channel model** | The FlexNode object model: one axis + typed channel slots, mapped onto register space. | **new, §3** |
| **L3 Fleet** | Discovery, two-lane scheduling, time sync, host-loss autonomy. | **new, §5, §7** |

**We do not invent a protocol.** L0 and L1 stay bit-for-bit stock moteus, which buys `moteus_tool`, `tview`, the CAN bootloader, and the Python library for free. FlexNode is entirely an L2/L3 addition living in unused register space. A stock moteus host talking to a FlexNode sees a working moteus controller; it simply never asks about the extra registers.

---

## 2. The object model

```
FlexNode
├── Axis 0        onboard BLDC via DRV8353S      → registers 0x000–0x07F   (stock moteus, untouched)
├── Node services identity, caps, health, fleet   → registers 0x080–0x0FF   (v1 block, retained)
├── Channel 1..8  typed peripheral slots          → registers 0x200 + 0x10·n (new)
└── Tunnel        bulk transfers (ToF blob, logs) → diagnostic stream channel
```

Axis 0 may be **absent** (a node with no motor is a legitimate configuration, and a cheap one — it drops the whole FOC stack). Channels are what the user's "actuator + x configuration" actually maps onto.

---

## 3. The channel model

### 3.1 Why slots

The v1 map hard-assigned registers per peripheral kind: load cell here, servo there, encoder over there. That works for a fixed robot and fails for a product. "Any accommodatable combination of servos, load cells, encoders, a SimpleFOC driver, ToF and IMU" has too many combinations to enumerate, and every new peripheral would need a new register range and a new host-side special case.

A slot model inverts it: **the host discovers a list of typed channels and drives them all through one uniform interface.** Adding a peripheral type adds an enum value, not a register range. The host's channel driver is written once.

### 3.2 Channel type enum

`uint8`. **Stable API — values are never reused or renumbered.** Grouped by decade so related types stay adjacent.

| Value | Type | Direction | Resources used |
|---|---|---|---|
| `0x00` | `none` | — | slot empty |
| `0x10` | `servo_rc` | out + optional fb | pulse-capable pin, 5 V rail, optional angle sensor |
| `0x11` | `bldc_ext` | out + fb | 3× PWM + enable, angle sensor — **see §9.2** |
| `0x20` | `enc_i2c` | in | I²C address (AS5600 / AS5600L / MT6701) |
| `0x21` | `enc_spi` | in | SPI + CS (secondary absolute encoder) |
| `0x30` | `loadcell` | in | external ADC (NAU7802 I²C recommended, HX711 2-wire, ADS1220 SPI) |
| `0x31` | `adc_in` | in | ADC-capable pin |
| `0x40` | `imu` | in | onboard LSM6DS3TR-C |
| `0x41` | `tof` | in | VL53L7CX / VL53L5CX |
| `0x50` | `pixel` | out | WS2812 data pin |
| `0x60` | `gpio_in` | in | any GPIO |
| `0x61` | `gpio_out` | out | any GPIO |
| `0x70` | `rail_5v` | in | PB11 current sense |

### 3.3 Channel register frame

Base `0x200`, stride `0x10`. **Sixteen slots of address space are reserved** (`0x200`–`0x2FF`),
even though v1.0 hardware can populate at most about eight — address space is free, and running out
of it later would force exactly the kind of layout break §10 promises never to make. Slot 0 is
reserved as a future alias for the onboard axis.

Every channel of every type has the same ten core registers:

| Offset | Name | R/W | Meaning |
|---|---|---|---|
| `+0x0` | `type` | R | channel type enum; `0x00` = slot empty |
| `+0x1` | `caps` | R | per-type capability bits (has_feedback, has_current, is_calibrated, …) |
| `+0x2` | `status` | R | per-channel fault/health bits; 0 = healthy |
| `+0x3` | `mode` | R/W | `0` released/safe (**boot default**), `1` active, ≥2 type-specific |
| `+0x4` | `cmd_a` | R/W | primary target — angle°, position rev, duty, level, colour |
| `+0x5` | `cmd_b` | R/W | rate limit — °/s, rev/s |
| `+0x6` | `cmd_c` | R/W | effort limit — current A, voltage, brightness |
| `+0x7` | `meas_a` | R | measured primary — angle, distance, force |
| `+0x8` | `meas_b` | R | **second loop-critical measurement** for this type — kept adjacent to `meas_a` so the fast lane can read both in a 2-register subframe |
| `+0x9` | `meas_c` | R | measured effort — current, raw counts |
| `+0xA` | `meas_d` | R | auxiliary — temperature, magnet AGC, signal quality |
| `+0xB` | `nonce` | R | increments once per update; **staleness detection** |
| `+0xC`–`+0xF` | type-specific | | e.g. tare command, ToF quadrant minima, pixel index |

**The stride is the whole point.** Because subframes address *consecutive* registers:

- a complete channel command is **one write subframe**: 3× int16 at `+0x4`
- a complete channel readback is **one read subframe**: 4× int16 at `+0x7`
- a *fast-lane* readback is cheaper still: **2× int16 at `+0x7`** — 3 B request / 7 B reply against
  4 B / 12 B for the full four. Per node per cycle that is often a whole DLC step (§5.2), which is
  why every type puts its loop-critical pair in `meas_a`/`meas_b`

So a node with two active channels adds roughly `2 × (3 + 3·2)` command bytes and `2 × (3 + 4·2)` reply bytes to its frame — about 18 and 22 bytes. That is affordable at gait rate; [§5](#5-cadence-and-bus-budget) does the arithmetic.

### 3.4 Type-specific bindings

Only the interpretation changes; the frame does not.

| Type | `cmd_a` | `cmd_b` | `cmd_c` | `meas_a` | `meas_b` | `meas_c` | `meas_d` |
|---|---|---|---|---|---|---|---|
| `servo_rc` | angle ° | slew °/s | — | angle ° (from bound encoder) | °/s | 5 V current A | — |
| `bldc_ext` | position rev | velocity rev/s | current limit A | position rev | velocity rev/s | current A | driver temp |
| `enc_i2c` | — | — | — | angle ° | °/s | — | magnet AGC / health |
| `loadcell` | — | — | — | force (scaled) | **contact flag** | raw counts | d/dt |
| `imu` | — | — | — | — | — | — | temp |
| `tof` | — | — | — | min distance mm | — | target count | ambient |
| `pixel` | mode | — | brightness | — | — | — | — |
| `rail_5v` | — | — | — | current A | — | peak A | — |

> **The IMU is a deliberate exception.** Six axes do not fit `meas_a`–`meas_d`, so the `imu`
> channel carries only the descriptor rows (`type`, `caps`, `status`, `nonce`) and the data stays
> in the well-known block at `0x098`–`0x09F`, where it is already implemented and already reads as
> six consecutive registers in one subframe. The channel entry exists so the host can *discover*
> the IMU uniformly; it is not the transport for it. Same applies to any future wide sensor.

**Servo closed-loop is on-node.** `servo_rc` with a bound `enc_i2c` channel runs its own outer position loop against the measured angle, which is precisely the "offloading minor calculations to flexnode" the design calls for — the host writes a target angle at gait rate and the node handles slew limiting, the pulse train, and stall detection from the 5 V current. Binding is config (`flexnode.ch<N>.feedback_ch`), not protocol.

### 3.5 Resolution discipline

moteus's mapped-value scheme means int8/int16/int32/float are four *views* of the same register, differing only in precision. Fixed scales per channel type, chosen so int16 is always sufficient for control and int8 is usable for telemetry:

| Quantity | int8 | int16 | int32 |
|---|---|---|---|
| angle ° | 2 | 0.05 | 0.0005 |
| rate °/s | 10 | 0.5 | 0.005 |
| position rev | 0.1 | 0.001 | 0.00001 |
| current A | 0.5 | 0.02 | 0.0002 |
| force N | 5 | 0.1 | 0.001 |
| distance mm | 20 | 1 | 0.01 |

Use int16 on the fast lane; it costs 2 bytes and is finer than any of these sensors is accurate.

---

## 4. The node block 0x080–0x0FF (v1, retained)

**The v1 block does not change.** It is flashed, documented, and public. In v2 it is reinterpreted as the *well-known view*: a set of convenience aliases onto the first channel of each type, plus the node header. A host that only ever wants "the IMU" reads `0x098–0x09D` and never touches the channel model; a host that wants to enumerate an arbitrary node uses [§3](#3-the-channel-model). Both are supported forever.

Previously-reserved registers now defined in the node header:

| Reg | Name | R/W | Notes |
|---|---|---|---|
| `0x080` | capabilities bitmask | R | *retained.* bit0 load cell, bit1 I²C encoder, bit2 IMU, bit3 ToF, bit4 pixel, bit5 servo, bit6 5 V sense |
| `0x081` | node status/fault summary | R | *retained* |
| `0x082` | 5 V rail current | R | *retained* |
| `0x083` | **profile id** | R | which build is flashed — see [§6](#6-profiles-build-time-roles) |
| `0x084` | **build hash (low 16)** | R | host asserts the fleet is running the image it expects |
| `0x085` | **highest populated slot** | R | *not* a count — slots may be sparse, so a count would make the host miss a populated slot above an empty one. 0 → no channels; skip enumeration |
| `0x086` | **fleet role / master state** | R | see [§7](#7-host-loss-and-inter-node-autonomy) |
| `0x087` | **host-loss state** | R | 0 nominal, 1 warning, 2 timed out, 3 deputy-commanded |
| `0x088`–`0x0BF` | peripheral aliases | | *retained* — load cell, encoder, IMU, ToF, pixel, servo |
| `0x0FF` | **block version → 2** | R | was 1; bump signals the channel model is present |
| `0x0C0`–`0x0C3` | **node name** | R | 16 UTF-8 bytes, NUL-padded, int32 only — see below |

> **Correction to v1:** the v1 table lists `0x09E` as "IMU temperature". The LSM6DS3TR-C temperature register is die temperature, not motor or ambient temperature, and is only loosely useful. Keep the register, but do not let it be mistaken for a thermal-protection input — that is `0x00E` (FET temp) and the motor NTC.

---

### 4.1 Node identity — a name, not just a number

Aditya, 2026-09-07: *"each flexnode should also have a local id/name, for example the 2x shoulder
8108-8 flexnodes each have the gim 8108-8 connected to it, and a simplefocmini, and one of them
also have a ds3235 servo attatched, the other has free functionality."*

A numeric CAN id is an address, not an identity. Two nodes with the same silicon, the same profile
and the same channel list can still be *the front-left shoulder* and *the front-right shoulder*,
and every log line, fault report and calibration file wants to say which. So a node carries three
distinct things, and they should not be conflated:

| | What it is | Where | Changes when |
|---|---|---|---|
| **CAN id** | bus address, 1–127 | `id.id` config | rewired or re-addressed |
| **UUID** | immutable silicon serial | registers `0x150`–`0x153` | never |
| **Name** | human-meaningful role | `flexnode.name` config → registers `0x0C0`–`0x0C3` | the board is moved to a different joint |

`flexnode.name` is a **16-byte NUL-padded UTF-8 string** exposed as four consecutive int32
registers, deliberately mirroring how moteus already publishes its UUID at `0x150`–`0x153`. Four
consecutive registers means the host reads a whole name in **one subframe**, once, at enumeration.
Sixteen bytes is enough for `shoulder_fl`, `knee_rr`, `spine_mid`, and short enough to never
tempt anyone into putting a description there.

Why registers and not just a config string readable over the text protocol: the text protocol is
slow, is not available to a minimal host, and would make the name unavailable exactly when it is
most wanted — in a fault dump. Sixteen bytes of register space is a cheap price for every log line
being legible.

**The host should assert the mapping.** At enumeration it reads CAN id, UUID and name together and
checks them against its fleet manifest. A board swapped between joints without its name being
updated is then caught immediately, instead of showing up as a limp three weeks later. This pairs
with the profile/build-hash check in [§6.4](#64-the-rule-that-keeps-this-honest) — same idea,
same moment, one refusal.

---

## 5. Cadence and bus budget

### 5.1 Two lanes, one schedule

Unchanged in principle from v1, extended to channels:

- **Fast lane, every cycle:** per node, one frame carrying command + query. Contains the stock axis command (`0x000`, `0x020`–`0x022`), the stock axis query (`0x000`–`0x003`, `0x00D`–`0x00F`), plus each *fast* channel's command and readback subframes.
- **Slow lane, rotating:** one node per cycle additionally reads its full channel set, node header, and health. At 13 nodes and 325 Hz that is a **25 Hz refresh per node** — ample for IMU bias tracking, ToF, rail current and diagnostics.
- **Fire-and-forget:** pixel writes, tare commands, servo re-arm. No reply bit, dropped into idle slots.
- **Promotion is free.** Moving a channel from slow to fast is a host-side schedule change. Firmware is untouched.

### 5.2 Frame time

CAN-FD, BRS, 5 Mbit data: ≈ 30 µs of arbitration-rate fields + `(8·N + 43)` bits at 5 Mbit for an
N-byte payload — **plus two corrections an earlier revision of this section missed.**

> **Correction, 2026-09-07.** The budget below was optimistic by 15–20 points. Two effects, both
> verified in source:
>
> 1. **Payloads are DLC-quantised.** CAN-FD has no 33-byte frame — `fw/fdcan.cc:24 RoundUpDlc`
>    rounds up to the next legal size, so **anything from 33 to 48 B costs a full 48 B** on the
>    wire. A payload that grows by one byte past a step boundary costs a whole step.
> 2. **Fast-lane command frames are 29-bit extended, not 11-bit.** The host sets `0x8000`
>    (reply-requested) in the id (`lib/python/moteus/transport.py:344`), and the firmware sends any
>    id ≥ 2048 as extended (`fw/fdcan.cc:323`). Replies from nodes with id ≥ 8 are extended too.
>    That is **≈ +20 µs per frame** at 1 Mbit arbitration — paid on every frame, every cycle.
>
> **Both headline conclusions survive and are stronger than stated**, so the design does not change
> — but plan against the corrected numbers, not the old ones.

| Payload | Frame time (11-bit) | Frame time (29-bit) |
|---|---|---|
| 16 B | ~64 µs | ~84 µs |
| 24 B | ~77 µs | ~97 µs |
| 32 B | ~90 µs | ~110 µs |
| 48 B | ~116 µs | ~136 µs |
| 64 B | ~141 µs | ~161 µs |

### 5.3 Budget by configuration

Cycle cost = Σ over nodes (command frame + reply frame) + one slow-lane pair, at extended-frame
timing with DLC quantisation applied.

| Configuration | Corrected load | (old, wrong) |
|---|---|---|
| 13 nodes, axis only, 325 Hz | **~70 %** ⚠ | 55 % |
| 13 nodes, axis + 1 fast channel, 325 Hz | **~84 %** ✗ | 65 % |
| 13 nodes, axis + 2 fast channels, 325 Hz | **~97 %** ✗ | 75 % |
| 2 chains × 7, axis + 2 fast channels, 400 Hz | ~65 % ⚠ | 52 % |
| 2 chains × 7, axis + 2 fast channels, 1 kHz | ✗✗ | 130 % |

**Rules that fall out of this table — unchanged in direction, sharper in degree:**

1. Keep steady-state load ≤ 65 %. Headroom absorbs error frames, retransmission, and the slow lane
   landing on a fat node.
2. **Two fast channels per node is the ceiling on a single 13-node chain** — and on the corrected
   numbers even *one* fast channel is uncomfortable there. Budget them deliberately: foot contact
   and joint angle earn their place; IMU at gait rate usually does not.
3. **1 kHz is not reachable on one chain and is not reachable on two.** 400 Hz on two chains is the
   real target. The RC servos are 50–330 Hz devices regardless, and FOC is local at 30 kHz either
   way.
4. **Splitting front/rear is no longer optional if more than the axis rides the fast lane.** It buys
   ~2× and independently buys fault containment — the single highest-leverage topology decision, and
   the corrected numbers make it close to mandatory.
5. **Watch the DLC step boundaries.** Because 33 B and 48 B cost the same, there is a free byte
   allowance up to each boundary and a cliff just past it. Design frames to land just under 16, 24,
   32 or 48 B.

---

## 6. Profiles (build-time roles)

### 6.1 The principle

The flash budget forces specialisation; the product forbids semantic drift. Both are satisfied by one rule:

> **A profile changes what is compiled in. It never changes what a register means.** A register whose feature is not compiled answers `kUnknownRegister`, exactly as an unpopulated peripheral does.

The host therefore cannot tell — and does not care — whether a channel is missing because the hardware isn't fitted or because the feature wasn't built. It reads `0x080`/`0x085`/`type` and composes its query. **One host implementation drives every profile.**

### 6.2 The profiles

| Profile | id | Axis 0 | Channels compiled | Use |
|---|---|---|---|---|
| `full` | 1 | BLDC | all types | dev board, bench, retail default |
| `joint` | 2 | BLDC | `enc_i2c`, `loadcell`, `imu`, `pixel` | CATBOT hip/knee — the 10 actuator nodes |
| `aux` | 3 | **none** | `servo_rc`×2, `enc_i2c`×2, `loadcell`, `imu`, `pixel`, `gpio` | spine/tail/head servo nodes |
| `bldc_ext` | 4 | BLDC | `bldc_ext`, `enc_i2c`×2, `imu`, `pixel` | head gimbal via SimpleFOC driver |
| `sense` | 5 | **none** | `imu`, `tof`, `loadcell`, `enc_i2c`, `pixel` | pure sensor node |

Implementation is a single `fw/flexnode_profile.h` selected by a Bazel flag, defining `MOTEUS_FLEXNODE_CH_*` macros. Channel drivers are compiled in or out; the register dispatch table is built from the same macros, so an absent driver costs zero bytes and zero cycles.

### 6.3 Where the space actually is

**Measured 2026-09-07 — see [flash-budget.md](flash-budget.md).** The result overturned the
assumption this section originally carried, twice.

The best lever costs **no code at all**: there is a **48,680 B hole in the flash map** between the
472 B vector table and the CAN bootloader at 0x0800C000 — `fw/stm32g474.ld` even labels part of it
"currently unused". Moving `*(.rodata*)` (23,102 B) into the `.isr_vector` output section reclaims
it, and `moteus_tool`, the bootloader and the export scripts all keep working unchanged because
they operate on that section by name.

Ordered levers, with measured bytes:

1. **Linker-script gap — ~23 kB immediately, ~47 kB available.** No code change.
2. **newlib-nano** + `-u _printf_float` — 28 kB exposed, saving unmeasured.
3. **C++ throw-stub chain** — 11,357 B ceiling, severed by one FlexNode-owned file.
4. **Measured unused drivers** — iC-PZ 6,637, UART `kSerial` 5,917, BiSS-C 3,832, quadrature 1,816,
   MA732 1,040 = **19,242 B**.
5. **Motor-less profiles** — largest of all (`bldc_servo.o` is 104.5 kB) and the only lever
   touching the motor path. **Not before hardware validation**, and now almost certainly never
   needed for space.

**Realistic total without going near the drive path: ≥42 kB**, against 16,848 B free today.

Also measured, and worth knowing even though the droppable subset is small: **18 % of the image
(79,518 B) is mjlib serialisation boilerplate** — every `PersistentConfig` / `TelemetryManager`
registration costs 2–8 kB. The tell was `ws2812_led.o` at 13.8 kB for a driver whose logic is
3.9 kB. Most registrations turn out to be load-bearing (`conf set` writes through them), so this is
a fact to design against rather than a lever to pull.

> **This demotes profiles from necessity to choice.** The entire v2 channel model, load cell,
> servo and ToF work fits in one image with room to spare. Profiles stay because a motor-less node
> that never links the FOC stack is a *safety property* and a product decision — but they are no
> longer a byte-count workaround, and `full` is comfortably plausible as the retail default.

### 6.4 The rule that keeps this honest

Every node reports its profile id (`0x083`) and build hash (`0x084`). The host asserts both at enumeration and refuses to run a fleet that isn't the image it was tested against. Mixed-image fleets are the failure mode that makes build-time specialisation dangerous, and this is the cheap defence.

---

## 7. Host-loss and inter-node autonomy

The Jetson is explicitly "allowed to be slow, and allowed to crash". A 4.8 kg machine standing on ten torque-producing joints needs a defined answer to *the commands stopped arriving*, and that answer must not depend on the thing that just died.

Three graded levels. **G0 is always on. G1 and G2 are opt-in and default off.**

### G0 — Independent timeout (stock moteus, always active)

Each node already runs a command watchdog: `servo.default_timeout_s` (default 0.1 s) drops the axis into **timeout mode (11)** with configured behaviour. No new code, no bus traffic, no coordination, no shared failure mode. Extended only by config: per-node timeout action (hold position / relax / current-limited hold) and the same for each channel — channels go to `mode = 0` (released) which is why released is the boot default.

**This is the floor and it is never disabled.** Everything below is an optimisation on top of a system that is already safe.

### G1 — Coordinated safe state (broadcast, no election)

The brainstem STM32 — which "stays alive when the Jetson does not" — emits a **presence beacon**: a fire-and-forget broadcast to destination `0x7f` at ~10 Hz carrying a sequence counter and a fleet-state byte. Nodes track beacon age in `0x087`.

- Beacon fresh → nominal.
- Beacon stale beyond `flexnode.fleet.warn_ms` → warning; node reduces limits, LED indicates.
- A single broadcast write can command the whole fleet into a named posture in one frame.

Cost is one 8-byte broadcast frame per 100 ms — under 0.1 % bus load. Requires no election, no node-to-node protocol, and no unsolicited transmission by any node. **If the brainstem is healthy, G1 is sufficient and G2 should stay off.**

### G2 — Deputy (config-gated; the only unsolicited transmit)

For the case where the brainstem *also* fails. Each node has `flexnode.fleet.deputy_rank` (0 = never a deputy; **factory default 0**).

1. Beacon age exceeds `fail_ms` → every node is already in G0 timeout, holding safe.
2. Each candidate waits `rank × stagger_ms`. Staggering, not arbitration, prevents two deputies.
3. The first to expire begins broadcasting to `0x7f`: a heartbeat plus a **canned safe sequence** — fold, lower, relax.
4. Any traffic from the host, the brainstem, or a lower rank **instantly demotes** it. Host authority always wins; no negotiation.
5. Deputy authority **expires** after `authority_ms`. On expiry everything falls back to G0. A deputy cannot rule indefinitely.

Nodes need no changes to accept this: moteus does not inspect the source field, so a deputy's frames are ordinary commands.

**The constraints that make G2 acceptable:**

- The deputy command whitelist contains only *reducing* actions. It can fold, lower, relax, and hold. **It cannot walk, and it cannot raise a limit.** A robot whose host has died should get smaller and lower, never more energetic.
- Default off, per-node, with an explicit rank. Nothing self-promotes out of the box.
- It is the single documented exception to poll-response, bounded by a timeout, an authority expiry, and a whitelist. The invariant is *scoped*, not abandoned.
- Ordinary bus load: **zero**. G2 transmits only when the fleet is already in a failure state.

> Build G0 now (it is free), G1 when the brainstem firmware exists, G2 only once CATBOT actually stands. **G2 is the most dangerous idea in this document** — a node that can command its peers is a node that can command its peers when it is *wrong*. The whitelist and the expiry are what make it survivable, and neither should ever be relaxed for convenience.

### Time sync

Fusing thirteen IMUs eventually needs common time. A broadcast sync tick to `0x7f` lets each node latch an offset against its millisecond counter (`0x070`); host-side timestamping is adequate at slow-lane rates. **Defer until fusion actually needs it** — it is cheap to add and pointless to build early.

---

## 8. Discovery and enumeration

The boot sequence a host runs, and the reason profiles cost the host nothing:

```
1. for id in 1..127:  read 0x100 (model), 0x150-0x153 (UUID)   → who exists
2. for each node:     read 0x0FF                               → FlexNode block version
                        0  → stock moteus, drive as such
                        1  → v1 block, fixed peripherals only
                        2  → v2, continue below
3. for each node:     read 0x080, 0x083, 0x084, 0x085          → caps, profile, build, channel count
4. assert profile and build hash match the fleet manifest       → refuse mixed images
5. for slot in 1..channel_count: read 0x200+0x10·slot +0..+1    → type, caps
6. compose that node's fast-lane query from its channel list
7. write per-channel config, arm channels (mode = 1)
8. start the schedule
```

Steps 1–7 happen once at boot and cost milliseconds. Nothing in the steady-state loop re-discovers anything.

---

## 9. Hardware gaps that this design exposes

Writing the channel model made three v1.0 silicon limits concrete. **These are hardware findings, not protocol problems** — the protocol above is correct regardless; what changes is which channels a v1.0 board can actually populate.

### 9.1 One aux output pin — and it can be hardware-timed after all

FlexNode v1.0 brings out **PC13** as its servo/LED output. In the family-0 aux table
(`fw/moteus_controller.cc:406`) PC13 has no timer, no ADC, no I²C and no SPI entry.

> **Correction, 2026-09-07.** An earlier revision reported "PC13 has no timer" as a property of the
> silicon. **It is not — it is a property of the moteus aux table.** On the G474, `PC_13` maps to
> **TIM1_CH1N (AF4)** and **TIM8_CH4N (AF6)** (`TARGET_STM32G474xE/.../PeripheralPins.c`). A
> **hardware-timed** servo pulse is available via **TIM8_CH4N**, which is unclaimed and — unlike
> TIM1_CH1N — does not collide with the SimpleFOC path in §9.2. Software timing is a fallback, not
> a requirement.
>
> The RTC-domain caveats still stand: PC13 is weak (~3 mA) and slow-slewing. Fine for a servo pulse
> or a few LEDs; buffer it for long or fast strings.

> **Withdrawn recommendation.** An earlier revision of this section called this a real problem and
> recommended a **PCA9685** I²C PWM expander to get seven servos onto one node. Aditya, 2026-09-07:
> **the 7 servos hang off 7 *different* FlexNodes, never two on the same one** — the mechanical
> layout was spaced that way from the beginning. One software-timed pulse per node is therefore
> sufficient, and the expander solved a problem that does not exist. The lesson is to ask how the
> mechanical layout distributes load before optimising a per-node resource.

**PC13 is also not the only option.** Aditya, 2026-09-07: *"the i2c pins can be repurposed as gpio
if the lsm6dtr is not used — didnt think that one out i think desoldering it works though. i2c
fallback gpio expansion is always an option."* So per-node I/O is a **trade**, not a fixed budget:

| Keep | Give up | Gain |
|---|---|---|
| I²C on PB8/PB9 | those pins as GPIO | IMU, ToF, I²C encoders, I²C load-cell ADC |
| PB8/PB9 as GPIO (desolder the LSM6DS3TR-C) | IMU, ToF, all I²C peripherals | 2 free GPIOs |
| I²C **plus** an expander | nothing | many PWM/GPIO channels on wires that already exist |

The expander stays available for a future node that genuinely needs more channels than v1.0 breaks
out. It just isn't needed for the servos.

**This is exactly why capabilities are discovered rather than assumed.** Two nodes with identical
silicon can differ by a desoldering operation, so a node's populated channel list is genuinely
per-node — read from register 0x080 and the channel `type` registers, never inferred from a part
number.

### 9.2 The SimpleFOC driver cannot be driven by v1.0 as specified

A SimpleFOC Mini (DRV8313-class) needs **3 PWM + enable**; the dual driver for the head needs two sets. PC13 cannot provide this.

**This got more urgent, not less.** Aditya, 2026-09-07: each of the two **shoulder nodes carries a GIM 8108-8 on its main axis *and* a SimpleFOC Mini** (one of the two also has a DS3235 servo; the other has spare capacity for load cell / RGB). So `bldc_ext` is not an alternative to the onboard FOC axis — it is an **addition to it, on the same node, at the same time**. That is a second commutation path plus I²C feedback polling on a core already running a 30 kHz FOC ISR: a CPU question as much as a flash question.

There is a *candidate* path, and it now checks out on paper. **PB13/PB14/PB15** — the SPI2
expansion pads — map to **TIM1_CH1N (AF6) / CH2N (AF6) / CH3N (AF4)**, confirmed in
`TARGET_STM32G474xE/.../PeripheralPins.c`. Three hardware PWM channels from one timer is exactly
what a Mini wants.

> **Correction, 2026-09-07: FlexNode's motor PWM is on TIM5, not TIM2.** An earlier revision said
> TIM2. `moteus_hw.h` defaults `pwm1/2/3 = PA_0_ALT0 / PA_1_ALT0 / PA_2_ALT0`, and on the G474 map
> `PA_x_ALT0` is **TIM5_CHx (AF2)** — plain `PA_x` would be TIM2 (AF1). TIM5 is also the CAN
> bootloader's time base (`bootloader.h:36`). The conclusion survives, but the reasoning was wrong
> and would have misled anyone auditing timer allocation.

**Timer allocation, audited.** Claimed: **TIM5** (motor PWM + bootloader), **TIM15** (mbed
`us_ticker` and `MillisecondTimer`), **LPTIM1** (ADC trigger, `bldc_servo.cc:685`); TIM2/TIM3 only
if aux1 hardware quadrature is configured; TIM4 only in non-FlexNode families. **Unreferenced and
therefore free: TIM1, TIM8, TIM16, TIM17, TIM20.**

Four constraints, none fatal but all easy to trip over:

- **`PB_14` is in FlexNode's aux1 pin table** (slot 1, ADC channel 5). It must be configured `kNC`
  or the aux port and the Mini will fight over it.
- **CHxN-only drive** needs `CCxNE`/`CCxNP` plus `MOE` — complementary outputs are not enabled by
  the ordinary channel-enable path.
- **TIM1's break interrupt shares a vector with TIM15** (`TIM1_BRK_TIM15_IRQn`), which mbed uses.
  Do not enable TIM1 BRK interrupts.
- **A shoulder node with both a Mini and a DS3235 cannot put the servo on TIM1_CH1N/PC13** — same
  channel as PB13. Use **TIM8_CH4N** for the servo, per §9.1.

Pad presence on the CEU6 is still Aditya's netlist to confirm.

**Costs:**

- It consumes the SPI expansion pads. SPI peripherals and the `bldc_ext` channel become mutually exclusive.
- A **second commutation loop** — open-loop or lightly closed against the bound `enc_i2c`, not a
  second FOC stack. Budgeted at **~10–13 kB** ([flash-budget.md](flash-budget.md)), which now fits
  comfortably. **Write it separate; share nothing with `bldc_servo`.**

> ⚠ **The CORDIC is a single shared peripheral**, used inside the FOC ISR with a write→read
> sequence. A main-loop user gets pre-empted mid-transaction and reads someone else's result. The
> Mini path must use **software sin/cos or a lookup table — never the shared CORDIC.** This is the
> kind of bug that shows up as rare, unreproducible torque glitches on *both* axes.

- **CPU headroom is unmeasured** — there is no ISR cycle instrumentation. The one available metric
  is `system_info.idle_rate` (`system_info.cc:75`), readable over SWD now as a delta of
  `moteus::SystemInfo::idle_count`.

**Decision required.** Three options, in my order of preference:

1. **Head gimbals get their own small MCU and become their own CAN node.** The SimpleFOC dual driver already implies a controller; giving it a CAN interface makes it a peer that speaks the same register protocol. Costs a board, buys total independence, and does not touch FlexNode at all. *If that node speaks classic CAN rather than CAN-FD it cannot share this bus* — a classic-only node errors on FD frames. It must be FD, or it must sit on its own bus.
2. **`bldc_ext` on TIM1 via the SPI pads,** accepting the loss of SPI expansion and the firmware cost. TIM1 availability is now confirmed in firmware; **pad breakout on the real board is not.** Trace it first.
3. **Drop `bldc_ext` for v1.0** and revisit on a v1.1 that breaks out a proper 4-pin driver header.

### 9.3 One I²C bus carries everything

IMU + ToF + every I²C encoder + a load-cell ADC + a possible PCA9685 all share I²C1 (PB8/PB9). At 400 kHz an AS5600 angle read is ~150 µs and an IMU burst ~300 µs; a node polling both at 1 kHz spends ~45 % of the bus. Two mitigations, both cheap:

- The 2 kΩ pull-ups are already sized for **Fast-mode Plus (1 MHz)**. Running Fm+ cuts all of the above by 2.5×. Verify with a scope on the first assembled node.
- Poll rates need not match the control rate: an RC servo's outer loop at 200 Hz is plenty, and the IMU at 104 Hz is what the part is configured for anyway.

**Choose I²C parts to keep this bus short:** prefer **NAU7802** (I²C, 24-bit) for load cells over HX711 (2-wire bit-bang, needs two GPIOs that v1.0 does not have to spare) and over ADS1220 (SPI, contends with the same pads as §9.2).

### 9.4 ToF firmware blob

Unchanged from v1 and still the right answer: the VL53L7CX/L5CX needs an ~84 KB blob at every power-on. It cannot live in node flash. The host streams it over the **diagnostic tunnel** at boot and the node forwards it over I²C. Zero flash cost; ToF only initialises when a host is present, which is acceptable because ToF is useless without one. **Bench-validate tunnel throughput before committing** — 84 KB over the tunnel at gait-rate scheduling could take an unpleasantly long time, and thirteen nodes doing it serially at boot could be minutes.

---

## 10. Compatibility contract

What FlexNode promises, so the register map can be published:

1. **L0/L1 are stock moteus.** `moteus_tool`, `tview`, the Python library, and the CAN bootloader work against any FlexNode. `kRegisterMapVersion` stays 5.
2. **Register semantics are immutable.** A meaning, scale, or unit is never changed. Registers are deprecated (permanently reserved, answering `kUnknownRegister`), never recycled.
3. **`0x0FF` is the block version.** It increments only on layout change. A host reads it first and knows exactly what it is talking to. v0 = stock moteus, v1 = fixed peripherals, v2 = channel model.
4. **Type enum values are permanent.** New peripheral types take new values.
5. **Absence is uniform.** Not populated, not configured, and not compiled are indistinguishable to a host, and all answer `kUnknownRegister`.
6. **Boot state is safe.** Every channel boots to `mode = 0` (released). Nothing energises because a node powered up.

---

## 11. Build order

Each step is independently testable, and nothing here requires a motor to turn.

| # | Step | Gate |
|---|---|---|
| 1 | **Get a CAN adapter.** Everything below is blind without one. | — |
| 2 | Verify the v1 block over CAN: read `0x080`, `0x0FF`, IMU; write a pixel colour; check `bus_V` against a DMM. | adapter |
| 3 | Measure the flash map; cut unused encoder drivers with real numbers. | — |
| 4 | Channel infrastructure: dispatch, `type`/`caps`/`status`/`mode`, enumeration. No drivers yet — an empty node that enumerates correctly is the real milestone. | 3 |
| 5 | `enc_i2c` (AS5600 / MT6701) — first real channel, no actuation, safe to iterate. | 4, encoder in hand |
| 6 | `loadcell` via NAU7802, including on-node contact thresholding. | 4 |
| 7 | `servo_rc`, software-timed on PC13 — or on a PCA9685 per §9.1. | 4 |
| 8 | Profiles: `flexnode_profile.h`, the Bazel flag, `joint` and `aux` builds, `0x083`/`0x084`. | 3, 4 |
| 9 | G0 host-loss config; per-channel timeout actions. | 4 |
| 10 | Multi-node bring-up: two nodes, distinct ids, enumeration, measured bus load against §5. | 2, 8 |
| 11 | G1 beacon, once brainstem firmware exists. | 10 |
| 12 | `bldc_ext`, `tof`, G2 deputy, time sync. | everything above, and §9.2 decided |

**Unchanged hard gates:** the AS5047 must be soldered before any position work, and **phase order must be validated on a current-limited supply before any calibration or sustained motor drive**. None of steps 1–12 requires violating either.

---

## 12. Open questions

1. **10 BLDC actuators vs 13 nodes** — what are the other three? Changes the profile mix, not the protocol.
2. **§9.2: how does the head gimbal driver connect?** Own CAN node (preferred), TIM1 on the SPI pads, or deferred to v1.1.
3. ~~PCA9685 for servos~~ — **closed 2026-09-07.** One servo per node, seven different nodes; PC13's software-timed pulse is sufficient. An I²C expander stays available for a future node that needs more channels than v1.0 breaks out.
4. **Load-cell ADC part** — NAU7802 recommended; decide when the foot design lands.
5. **Brainstem MCU** — the site says STM32G0 in one place and STM32G4 in another. G0 has no FDCAN; if the brainstem is to be the realtime bus master in Phase B it must be a G4 (3× FDCAN). Worth settling early, since it determines whether the two-chain topology in §5 is reachable at all.
6. **Is the Jetson on the CAN bus in production, or only via the brainstem?** Determines whether there are two masters and whether G1's beacon comes from one place or two.
