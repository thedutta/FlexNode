# CAN layer — why it is shaped this way

_Design decisions and their reasoning, recorded 2026-09-07 01:35–02:20 IST._

The spec itself is [`../docs/can-layer.md`](../docs/can-layer.md) (v2). This file holds the
*reasoning* — what was considered, what was rejected, and what would have to change to revisit a
decision. Read this before proposing a redesign.

## The framing that drives everything

Aditya, 2026-09-07: FlexNode exists to *"make the development of catbot infinitely easy with clever
engineering, and in the process get a sellable flexible module node too — and the can layer is a
core enabler of it."*

Two consequences, and nearly every decision below follows from one of them:

1. **The register map is a public, versioned API**, not an internal detail. It must not change
   meaning between boards, builds or firmware revisions.
2. **What a node *is* may vary; what a register *means* may not.**

## Decisions

### Extend moteus, don't invent a protocol — kept from v1

L0 (CAN-FD framing, `(source<<8)|dest`) and L1 (multiplex subframes) stay bit-for-bit stock. That
buys `moteus_tool`, `tview`, the CAN bootloader and the Python library for free, and it means a
stock moteus host sees a working moteus controller. FlexNode is purely an L2/L3 addition in unused
register space. **Revisit only if upstream claims 0x080–0x0FF**, which would be visible as a merge
conflict long before it broke anything.

### Typed channel slots, replacing v1's fixed per-peripheral registers

v1 hard-assigned registers per peripheral kind. That works for one fixed robot and fails for
*"any accommodatable combination of encoders, sensors or digital sensors."* Every new peripheral
would have needed a new register range and a new host-side special case.

Slots invert it: the host discovers a list of typed channels and drives them all through one
uniform 16-register frame at `0x200 + 0x10·slot`.

> The stride is the whole point, and it is worth understanding before touching the layout.
> Subframes address **consecutive** registers, so a full channel command is *one* write subframe
> (3× int16 at `+0x4`) and a full readback is *one* read subframe (4× int16 at `+0x7`). Reordering
> the frame to group fields "logically" would silently double the subframe count and the bus cost.
> **Do not reorder without redoing the bus budget.**

The v1 block 0x080–0x0FF is **retained verbatim** — it is flashed, documented and public — and
reinterpreted as the "well-known view", aliases onto the first channel of each type.

### One image per profile, config within a profile

v1 said "one image, thirteen configs". Aditya, 2026-09-07: *"hardware changes can neither be made
at runtime, so no hot-plugging and reflashes on hardware changes are fully acceptable."*

The rule that keeps this from wrecking the product story: **a profile changes what is compiled,
never what a register means.** An uncompiled feature answers `kUnknownRegister`, exactly as
unpopulated hardware does — so the host cannot tell the difference and does not care. One host
implementation drives every profile.

> Gotcha, 2026-09-07 02:05 IST: profiles were designed as the **release valve for the flash
> budget**, and then the flash budget turned out not to be a problem — see
> [`flash-findings.md`](flash-findings.md). **Profiles are no longer load-bearing for space.**
> They stay in the design because a motor-less node that never links the FOC stack is a *safety
> property* and a product decision. Don't let anyone reintroduce them as a byte-count argument.

Mixed-image fleets are the real hazard of build-time specialisation, so nodes report profile id
(0x083) and build hash (0x084) and the host refuses a fleet that isn't what it was tested against.

### Host-loss: three graded levels, G0 always on

The Jetson is explicitly *"allowed to crash"*. A 4.8 kg machine on ten torque-producing joints
needs a defined answer to *commands stopped arriving*, and that answer must not depend on the thing
that died.

- **G0** — stock `servo.default_timeout_s` watchdog → timeout mode. Free, no coordination, no
  shared failure mode. **Always on, never disabled.** Everything else is an optimisation on top of
  a system that is already safe.
- **G1** — brainstem presence beacon, fire-and-forget broadcast, ~10 Hz, <0.1 % bus. No election,
  no node-to-node protocol, no unsolicited transmission by any node.
- **G2** — config-gated deputy. The **only** unsolicited transmit in the design.

The enabling discovery, 2026-09-07 01:05 IST: **destination `0x7f` is a broadcast every node
already accepts** — `fw/moteus.cc:307`, filters 1 and 3 accept `prefix<<16 | 0x7f` for both
standard and extended frames. Nothing had to be invented. Also `can.prefix` namespaces a bus, which
is how two chains stay separate.

Moteus does not inspect the source field of a frame, so a deputy's commands are accepted as
ordinary commands with **no firmware change on the receiving side**.

> **G2 is the most dangerous idea in this design.** A node that can command its peers can do it
> while wrong. Three constraints make it survivable and **none of them may be relaxed for
> convenience**: (1) the command whitelist contains only *reducing* actions — it can fold, lower,
> relax, hold; it cannot walk and cannot raise a limit; (2) deputy authority **expires**, falling
> back to G0; (3) `deputy_rank` defaults to 0 on every node, so nothing self-promotes out of the
> box. Election is by **staggered rank**, not arbitration — simpler, and two deputies cannot
> coexist.
>
> Build order: G0 now, G1 when brainstem firmware exists, **G2 only once CATBOT actually stands.**

### Node identity is three separate things

CAN id (address), UUID (silicon serial, `0x150`–`0x153`, immutable), and **name** (human-meaningful
role). Aditya asked for the name on 2026-09-07. It is 16 NUL-padded UTF-8 bytes at `0x0C0`–`0x0C3`,
deliberately mirroring how moteus publishes its UUID — four consecutive registers, so a whole name
reads in one subframe.

Registers rather than a config string over the text protocol, because the text protocol is slow,
isn't available to a minimal host, and would make the name unavailable exactly when it is most
wanted: in a fault dump.

## Rejected

| Idea | Why not |
|---|---|
| A bespoke CATBOT packet format | Throws away moteus tooling and the bootloader for a bus that is only ~65 % loaded anyway |
| Nodes free-running telemetry at intervals | Unschedulable worst-case latency and arbitration storms. The master owns all timing; "report at intervals" is the *master's* schedule |
| Fixed per-peripheral registers (v1) | Doesn't survive arbitrary peripheral combinations |
| **PCA9685 expander for 7 servos** | **Withdrawn 2026-09-07** — the servos hang off 7 *different* nodes. Solved a problem that didn't exist. See [`hardware-io.md`](hardware-io.md) |
| 1 kHz on a single 13-node chain | Measured at 92–130 % bus load. Not reachable. 400 Hz on two chains is the right target |
| Fast-lane IMU on every node | Two fast channels per node is the practical ceiling; IMU belongs on the slow lane except on the 2–3 nodes feeding state estimation |

## Numbers worth not re-deriving

Frame time (CAN-FD, 11-bit id, BRS, 5 Mbit data): ≈30 µs of arbitration-rate fields + (8·N + 43)
bits at 5 Mbit. 16 B ≈ 64 µs, 32 B ≈ 90 µs, 64 B ≈ 141 µs.

| Configuration | Load |
|---|---|
| 13 nodes, axis + 1 fast channel, 325 Hz | 65 % ✓ |
| 13 nodes, axis + 2 fast channels, 325 Hz | 75 % ⚠ |
| 13 nodes, axis + 2 fast channels, 400 Hz | 92 % ✗ |
| 2 chains × 7, axis + 2 fast channels, 400 Hz | 52 % ✓ |
| 2 chains × 7, axis + 2 fast channels, 1 kHz | 130 % ✗ |

Keep steady-state ≤65 %. Splitting front/rear buys nearly 2× **and** fault containment — the
highest-leverage topology decision available.

## Still open

See [`open-questions.md`](open-questions.md). The two that block design work rather than
implementation:

1. **How does the SimpleFOC Mini attach?** Shoulder nodes need it *simultaneously with* their own
   GIM 8108-8, so it is an addition to the onboard FOC axis, not an alternative.
   [`hardware-io.md`](hardware-io.md) has the TIM1 candidate path.
2. **Brainstem: STM32G0 or G4?** **G0 has no FDCAN.** If the brainstem is to be the Phase-B
   realtime bus master it must be a G4. This decides whether the two-chain topology — the thing
   that buys 400 Hz — is reachable at all.
