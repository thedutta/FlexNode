# FlexNode — Distributed-Compute CAN Layer

_Design doc, 2026-07-21; status 2026-09-06: **partially implemented** — 0x080/0x081, IMU 0x098–0x09F, pixel 0x0B0–0x0B4 and 0x0FF are in firmware (see [firmware.md](firmware.md)), unverified over CAN. Companion to [firmware.md](firmware.md) and [roadmap.md](roadmap.md)._

CATBOT is a Jetson host plus up to 13 FlexNodes on CAN-FD. Every node closes its own FOC loop locally (15–30 kHz); the bus carries **commands down, telemetry up** at the gait-control rate. Beyond motor control, FlexNodes host peripherals — external RC servos (head / limb yaw / spine), AS5600L joint encoders, load cells (foot contact), NeoPixels, an on-board IMU (LSM6DS3TR-C), ToF (VL53L7CX), and 5 V rail current sense. This document defines how all of that rides one protocol.

## 1. Decision: extend the moteus register protocol, don't invent one

moteus already ships a register-based protocol over CAN-FD (1 Mbit arbitration / 5 Mbit data, ≤64 B payloads). Spec: `moteus-r4-parent/docs/protocol/can.md` + `registers.md`; reference parser: `lib/python/moteus/multiplex.py`.

A frame is a concatenation of **subframes**, each a read/write/reply of N *consecutive* registers at int8/int16/int32/float resolution:

```
host → node 1 (id 0x8001 = from 0x80|0, to 1, reply requested):
  01 00 0a          write 1×int8  reg 0x000 (Mode) = 10 (position)
  07 20 6000 2001 50ff   write 3×int16 regs 0x020.. (pos, vel, ff-torque)
  14 04 00          read  4×int16 regs 0x000-0x003 (mode, pos, vel, torque)
  13 0d             read  3×int8  regs 0x00d-0x00f (voltage, temp, fault)
node 1 → host: one reply frame with the requested values.
```

**"Dedicated CATBOT packet" vs "flexible per-node layout" is a false dichotomy here.** The **register map is fixed** — one universal FlexNode block (§5) with identical semantics on every node ever built — while **each frame is composed per node**: the host queries only the registers a given node populates. A node without a load cell answers `kUnknownRegister`; the capabilities register (0x080) lets the host autodiscover the fleet at boot and build each node's query automatically.

**One firmware image, thirteen configs.** Node identity is *data*, not code: CAN id (`id.id`), aux pin modes, encoder sources, and the new `flexnode.*` keys all live in moteus persistent config (`conf set` / `conf write`). One `.elf` is flashed to all nodes (SWD or CAN bootloader); per-node behavior comes from config. No per-node builds.

**Strictly poll-response.** Stock moteus firmware never transmits unsolicited, and FlexNode keeps that: the bus master owns all timing, worst-case latency is schedulable, and there are no arbitration storms. "Report at intervals" is implemented by the *master's* schedule (§3), not by nodes free-running.

## 2. Topology & host phases

Single-master daisy chain(s); terminate only the two physical chain ends (120 Ω), per [roadmap.md](roadmap.md).

| Phase | Master | Chains | Realistic rate | Notes |
|---|---|---|---|---|
| **A — bring-up** | Jetson + mjbots **fdcanusb** (USB) | 1 × 13 nodes | 250–350 Hz | Works with the stock `moteus` python lib immediately. USB adds ~0.1–0.5 ms jitter — fine for gait development. |
| **B — spinal cord** | **Core-board STM32G4** as realtime bridge | 2 chains (front legs + head / rear legs + spine), 6–7 nodes each | 400 Hz–1 kHz | G4 parts have **3× FDCAN**. Core board runs the hard-realtime poll loop; Jetson sends high-level targets over USB. Same pattern as mjbots' pi3hat (5 buses). Core board already exists (STM32 + battery telemetry + radios + OLED), so this is an extension, not a new board. |

The protocol and register map are identical in both phases — only the master moves.

## 3. Cadence model: two lanes, one schedule

Each control cycle the master sends every node **one frame** (command + query) and receives **one reply**:

- **Fast lane (every cycle):** position/velocity/torque command + query of mode, position, velocity, torque, voltage/temperature/fault — plus the node's *fast-lane peripherals*: **foot-contact flag (0x08B)** and **AS5600L angle** (via Encoder-2 regs 0x054/0x055, see §6). One extra int8/int16 in an existing subframe costs ~1–3 bytes.
- **Slow lane (rotating):** each cycle, *one* node's query additionally reads its full sensor block (IMU, ToF summary, rail current, health). 13-node rotation at 325 Hz ⇒ every node's slow block refreshes at ~25 Hz.
- **Fire-and-forget writes** (NeoPixel mode, servo angle targets, tare command): command frames without the reply bit, dropped into idle slots. No reply cost.
- **Promotion is free:** if the locomotion NN starts caring about a peripheral (e.g. spine servo state), add its registers to that node's fast query — a host-side change only; firmware is untouched.

Client side, this is already supported: `QueryResolution._extra` / `make_custom_query()` in `lib/python/moteus/moteus.py` read arbitrary registers in the same frame as the command.

## 4. Bus budget

Frame time model (CAN-FD, 11-bit id, BRS): ≈ 30 µs of 1 Mbit fields + (8·N + ~43) bits at 5 Mbit for an N-byte payload:

| Payload | Frame time |
|---|---|
| 16 B | ~64 µs |
| 24 B | ~77 µs |
| 32 B | ~90 µs |
| 64 B | ~141 µs |

Cycle cost = Σ per node (command frame + reply frame), plus one slow-lane pair:

| Configuration | Cycle cost | @ rate → utilization |
|---|---|---|
| 1 chain × 13, rich query (24 B + 24 B) | ~2.1 ms | 400 Hz → **83 % (too hot)** |
| 1 chain × 13, trimmed int16 (16 B + 16 B) + slow pair | ~1.9 ms | 325 Hz → ~62 % ✓ |
| 2 chains × 6–7, rich query | ~1.1 ms/chain | 400 Hz → ~45 % ✓ |
| 2 chains × 6–7, trimmed | ~0.9 ms/chain | 1 kHz → ~90 % (edge; suspend slow lane, 6/7 split) |

Rule of thumb: keep steady-state utilization ≤ 60 % so retransmissions and slow-lane bursts never break the cycle. Hence Phase A targets 250–350 Hz and Phase B makes 400 Hz comfortable.

**Rate guidance:** 400 Hz is ample for CATBOT's gaits — the external RC servos (head/yaw/spine) are 50–330 Hz devices regardless, and FOC runs locally. Revisit 1 kHz only if a future locomotion policy demands sub-ms torque transparency, and then only with Phase B dual chains.

## 5. The FlexNode register block — 0x080–0x0FF

Upstream moteus uses ≤ 0x07f for realtime registers and 0x100–0x158 for info/UUID; **0x080–0x0FF (128 registers) is free** and becomes the FlexNode block. Fixed offsets, identical on every node; unpopulated → `kUnknownRegister` reply. Registers ≥ 0x080 cost a 2-byte varuint address — one extra byte per subframe, negligible.

| Reg | Name | R/W | Notes |
|---|---|---|---|
| 0x080 | **Capabilities bitmask** | R | ✅ bit0 load cell, bit1 AS5600L, bit2 IMU (set when WHO_AM_I answers), bit3 ToF, bit4 NeoPixel, bit5 servo, bit6 5V-sense. Host autodiscovery. |
| 0x081 | FlexNode status/fault bits | R | peripheral fault summary (I²C errors, servo overcurrent, ToF not-initialized, …) |
| 0x082 | 5 V rail current | R | PB11 ADC, firmware zero-offset calibrated |
| 0x083–0x087 | _reserved_ | | |
| 0x088 | Load cell force (scaled) | R | int16 mapped via `flexnode.loadcell.scale` |
| 0x089 | Load cell raw | R | |
| 0x08A | Tare command / status | R/W | write 1 = tare now |
| **0x08B** | **Contact flag** | R | int8 0/1, thresholded **on-node** (`flexnode.loadcell.threshold`). Cheapest possible fast-lane read; enables local reflexes later. |
| 0x08C–0x08F | _reserved (2nd cell / hysteresis config)_ | | |
| 0x090 | AS5600L raw angle | R | primary angle path is Encoder 2 (§6) — this is the raw/aux view |
| 0x091 | AS5600L magnet/AGC health | R | AGC + MD/ML/MH bits |
| 0x092–0x097 | _reserved (2nd AS5600L slot)_ | | |
| 0x098–0x09A | IMU accel X/Y/Z | R | ✅ g: int8 0.1, int16 0.001, int32 1e-5, float; 6 consecutive regs ⇒ one read subframe |
| 0x09B–0x09D | IMU gyro X/Y/Z | R | ✅ dps: int8 1, int16 0.1, int32 0.001, float |
| 0x09E | IMU temperature | R | ✅ °C (moteus temperature scaling) |
| 0x09F | IMU status/nonce | R | ✅ increments per sample — staleness detection; 0 while IMU inactive |
| 0x0A0 | ToF min distance | R | mm |
| 0x0A1–0x0A4 | ToF quadrant min distances | R | 4 quadrants of the 8×8 grid |
| 0x0A5 | ToF target count | R | |
| 0x0A6 | ToF frame nonce | R | |
| 0x0A7–0x0AF | _reserved_ | | full 8×8 frame streams over the **diagnostic tunnel**, not registers |
| 0x0B0 | NeoPixel mode | R/W | ✅ 0 off · 1 solid (2 breathe · 3 chase · 4 custom(tunnel) reserved, currently treated as solid) |
| 0x0B1–0x0B3 | NeoPixel RGB | R/W | ✅ master colour 0..255 (int16+ on the wire); live, un-persisted — `conf set led.*` clears |
| 0x0B4 | NeoPixel brightness | R/W | ✅ 0..255 |
| 0x0B5–0x0B7 | _reserved (2nd color / rate)_ | | |
| 0x0B8 | Servo 1 angle command | R/W | calibrated degrees → pulse via `flexnode.servo.*` map; raw duty remains available at upstream aux-PWM regs 0x076–0x07f |
| 0x0B9 | Servo 2 angle command | R/W | |
| 0x0BA | Servo arm/disarm | R/W | 0 = outputs released (safe default at boot) |
| 0x0BB | Servo fault/overcurrent status | R | pairs with 0x082 |
| 0x0BC–0x0BF | _reserved_ | | |
| 0x0C0–0x0FE | _reserved for future FlexNode use_ | | |
| 0x0FF | FlexNode block version | R | ✅ = 1; bump on any layout change |

**Config namespace** (`conf set flexnode.…`, persisted like all moteus config): per-peripheral enables (drive the caps bitmask), `loadcell.scale/threshold/invert`, `servo.N.pulse_min/pulse_max/angle_min/angle_max`, `pixel.count`, `imu.rate_hz`, `tof.enable`.

### Fast/slow lane assignment

| Lane | Registers | Rate |
|---|---|---|
| Fast (every cycle) | 0x000–0x003, 0x00d–0x00f (stock) + 0x08B contact + 0x054/0x055 AS5600L | 250–400 Hz |
| Fast optional | 0x098–0x09D IMU (nodes feeding state estimation; or half-rate) | ≤ loop rate |
| Slow (rotating) | 0x080–0x082, 0x088–0x08A, 0x090–0x091, 0x098–0x09F, 0x0A0–0x0A6, 0x0BB | ~25 Hz/node |
| Fire-and-forget | 0x08A tare, 0x0B0–0x0B4 pixels, 0x0B8–0x0BA servos | as needed |

## 6. Reuse map — what already exists in moteus

| FlexNode need | Existing mechanism | Delta required |
|---|---|---|
| AS5600L angle at loop rate | I²C encoder as `motor_position` source → **Encoder 2 regs 0x054/0x055** (position + velocity, PLL-filtered) | upstream supports AS5600 only: add `kAs5600L` to `I2C::DeviceConfig::Type` (`fw/aux_common.h`) + programmable-address handling |
| External RC servos | aux pin `kPwmOutput`, regs 0x076–0x07f, `pwm_period_us` config | calibrated-angle wrapper regs 0x0B8+ (small) |
| Load cell (analog amp, e.g. HX711-less bridge amp) | aux pin `kAnalogInput`, regs 0x060+ | scaling/threshold/tare logic + regs 0x088+ (if HX711 chosen instead: new bit-bang driver — decide at bring-up) |
| GPIO odds and ends | aux GPIO command/status regs 0x05c–0x05f | none |
| Per-node config | `PersistentConfig` (`conf set/write`) | add `flexnode.*` structs |
| Big transfers (ToF frames, pixel patterns) | multiplex **diagnostic tunnel** (stream channel) | framing convention only |
| IMU, ToF, NeoPixel drivers | — none upstream — | net-new firmware (see roadmap) |

## 7. Flash budget & open items

~17 KiB free on the 512 KB CEU6 as of 2026-09-06, with the WS2812 driver, IMU and the register skeleton in ([firmware.md](firmware.md)). Load-cell, ToF-summary and servo wrappers must be written lean against existing primitives (aux I²C engine, aux ADC, aux PWM); the IMU tilt demo is the first thing to drop if space runs out.

- ⚠️ **VL53L7CX requires an ~84 KB firmware blob uploaded to the sensor at every power-on — it cannot live in node flash.** Design answer: the **host streams the blob over the CAN diagnostic tunnel at boot**, the node forwards it to the sensor over I²C. Zero flash cost; the trade is that ToF only initializes when a host is present (acceptable — ToF is useless without the host anyway). Bench-validate tunnel throughput for an acceptable boot time before committing.
- AS5600L enum + address extension (small, do with encoder bring-up).
- HX711 vs analog bridge amp for load cells — decide when the foot design lands; both paths documented above.
- Multi-node time sync (fusing 13 IMUs): stock millisecond-counter reg 0x070 + host-side timestamping is fine at 25 Hz; if fusion later needs tighter sync, a broadcast (id 0x7f) "sync tick" write is protocol-legal and cheap — defer until needed.
- `kRegisterMapVersion` stays 5 (upstream compatible); FlexNode block versioned separately at 0x0FF.
