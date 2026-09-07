# CATBOT CAN subsystem — design

_2026-09-08. Supersedes the fleet assumptions in [`docs/can-layer.md`](../docs/can-layer.md) (13 nodes, Jetson-or-brainstem master) with Aditya's corrected inputs of 2026-09-08: **10 FlexNodes, every one with its own BLDC axis, varying effectors, one corenode as bus master, Jetson behind the corenode, ~45 cm bus.** Builds on [`reports/2026-09-07-can-layer.md`](2026-09-07-can-layer.md). Where this document contradicts `docs/can-layer.md`, §12 lists the spec change._

> **Nothing here has been tested. There is no CAN adapter on the bench.** Every byte count is derived from the moteus source in this repo (file:line cited); every microsecond is arithmetic from the CAN FD frame format and the TCAN334G datasheet; every "recommended" value is untested. The three quantities that most need measuring before this is trusted are marked **[measure]**.

---

## 0. Bitrate is not loop rate — read this first

Aditya: *"I do not understand what Hz has to do with anything, I thought CAN-FD maxes out at 5 Mbps, so Hz is the clock speed of CAN?"*

There are two different numbers, and they live at different layers.

**Bitrate** is a property of the wire. CAN FD sends each frame in two phases: the *arbitration phase* at **1 Mbit/s** (one bit every 1 µs — slow, because every node must see every bit before the next starts so that two nodes starting together can resolve who wins), and the *data phase* at **5 Mbit/s** (one bit every 200 ns — fast, because by then exactly one node is transmitting). Both numbers are fixed by the transceiver (TCAN334G is a 5 Mbit part), the controller configuration (`fw/moteus.cc:213-214`: `slow_bitrate = 1000000; fast_bitrate = 5000000`) and the ISO standard. They say how long *one frame* takes on the wire. They say nothing about how often you send one.

**Loop rate in Hz** is a schedule the master chooses. One "cycle" means: the corenode sends every node its command and gets every node's reply. The loop rate is how many such cycles happen per second. It is bounded by the bitrate — you cannot complete a cycle faster than the sum of all its frames — but it is not the same thing, any more than a road's speed limit is the same as how many buses a day you run on it.

The derivation, end to end, with real numbers (details in §3 and §5):

| Step | Quantity | Value |
|---|---|---|
| 1 | Time for one 16-byte command frame on the wire (48 arbitration-rate bits at 1 Mbit + 161 data-rate bits at 5 Mbit) | **80 µs** |
| 2 | Time for the node's 12-byte reply | **74 µs** |
| 3 | Per-node round trip (command + reply), plain actuator node | **154 µs** |
| 4 | Ten nodes, using the actual mix in §5 (two heavy, six medium, two plain) | **1,704 µs** |
| 5 | Plus one slow-lane request+reply pair per cycle (§5.3) | **+246 µs → 1,950 µs per cycle** |
| 6 | Maximum loop rate if the bus were 100 % busy: 1 / 1.95 ms | **513 Hz** |
| 7 | Keep the bus ≤ 65–70 % busy (retransmissions, error frames, node turnaround jitter, bit stuffing): 513 × 0.65…0.70 | **330–360 Hz usable** |

So: **5 Mbit/s is the wire; ~350 Hz is the schedule; the bus is then about two-thirds occupied.** The coordinator's rough figures (≈190 µs per node, ≈1.9 ms per cycle, ≈515 Hz ceiling, 300–400 Hz usable) check out: per node it is 154–194 µs on the fast lane plus a 25 µs share of the slow lane, the cycle is 1.95 ms, the ceiling 513 Hz, and the usable band 300–385 Hz depending on how much margin you keep.

Two rates that are *not* the loop rate, for orientation: the FOC current loop inside each FlexNode runs at 30 kHz and never touches the bus; the Jetson's policy runs at whatever rate it runs and the corenode interpolates between its outputs at bus rate.

---

## 1. Topology

```
 Jetson Orin Nano Super ──(USB / UART / SPI — undecided, §11)── corenode (STM32G4, FDCAN1)
                                                                    │ J3
                                                          FlexNode 1 ── 2 ── 3 ── … ── 10
                                                          (120 Ω)                    (120 Ω)
```

- **One master.** The corenode owns every frame on the bus. FlexNodes transmit only in reply to a frame addressed to them (the single exception, if it is ever built, is §9).
- **10 nodes, ids 1–10**, daisy-chained J3-in/J4-out per [`docs/hardware.md`](../docs/hardware.md), terminated at the corenode end and at node 10 only.
- **Jetson behind the corenode.** The Jetson never appears on the CAN bus. The corenode is the only node that can be a G1 beacon source (§9) and the only thing that can enumerate the fleet.
- **Growth path:** a second chain on the corenode's FDCAN2 (the STM32G4 has three FDCAN instances). Not needed at 10 nodes (§6).

The corenode must therefore have FDCAN. **An STM32G0 cannot be the corenode** — it has no FDCAN peripheral. This settles the G0/G4 question raised in `open-questions.md` §8 as far as the bus master goes; whether a separate G0 exists for the zero-watt acoustic-wake duty is a power-architecture question, not a bus one.

---

## 2. Physical layer

### 2.1 Propagation over 45 cm: not a constraint

Signal velocity in twisted pair is ≈ 0.2 m/ns (≈ 5 ns/m). End to end over 45 cm: **≈ 2.3 ns one way, ≈ 4.5 ns round trip.**

- Against the **1 µs arbitration bit**: the CAN arbitration rule is that a dominant bit from the far end plus the transceiver loop delay must arrive before the local sample point. Round trip is 2 × (2.3 ns + 135 ns loop) ≈ 275 ns against a sample point at 671–800 ns (§2.4). **Margin ≈ 400–500 ns.** Length is 0.5 % of the budget; the transceiver is the rest.
- Against the **200 ns data bit**: only one node transmits, so propagation does not limit anything except the transmitter checking its own bit (§2.5).
- **Where length would start to matter.** Arbitration at 1 Mbit fails when 2 × (5 ns/m × L + 135 ns) exceeds roughly 600 ns, i.e. **L ≈ 30–40 m** — the textbook "40 m at 1 Mbit". At 5 Mbit the limit is not propagation but ringing: the TCAN334G differential edge is ≈ 17 ns (datasheet t_r), and a line is electrically "lumped" — reflections are invisible — while its round trip is well under the edge time. At 5 ns/m that is roughly **< 1 m total**, with stubs under ≈ 30 cm. CATBOT's 45 cm bus is comfortably inside. Past ~1–2 m of total wire, or stubs past ~30 cm, the data phase would start to need CiA 601-style stub discipline. *(Rule-of-thumb figures; the ISO/CiA guidance is not in this repo.)*

### 2.2 Star vs daisy-chain at 45 cm

**Would a star work?** Electrically, at this length, yes. A hub with ten ≤ 20 cm stubs has stub round trips of ≈ 2 ns, an order of magnitude under the 17 ns edge, so each stub's reflection is over before the driver has finished the edge. Total capacitive load — ten transceivers at ~20 pF plus ~1.5 m of wire at ~50 pF/m — is ≈ 300 pF, and the TCAN334G is characterised at 100 pF (60 Ω) and 200 pF (120 Ω). The star would sit somewhere between those two datasheet columns: functional, with the "highly loaded" loop-delay figure (180 ns max) becoming the relevant one — which matters for §2.5.

**Daisy-chain is still the right call, for reasons that are not convention:**

1. **The hardware already does it.** J3-in/J4-out is on every board; a star needs a hub board with ten connectors and twenty crimps that does not exist.
2. **Termination is unambiguous.** Two physical ends, two 120 Ω bridges. A star has no "ends" — you terminate the two longest branches and hope, which is exactly the kind of thing that works on the bench and fails at 40 °C.
3. **Wire mass and routing.** A chain along a limb is one pair going out and one coming back at each joint; a star is ten pairs converging on the torso.
4. **Lower total capacitance**, keeping the transceiver in its 100 pF column.

The one real cost of a chain: a break in the pair at joint *k* kills nodes *k+1…10* and leaves nodes *1…k* single-terminated. At 45 cm single-termination still works (§2.3), so the upstream half degrades rather than dies. That is the fault-containment argument for two chains later.

### 2.3 Termination: what actually goes wrong

Nominal: 120 Ω at each physical end, 60 Ω in parallel as seen by any driver.

- **A middle node also terminated (three resistors, 40 Ω).** The driver must source ~50 % more current to reach the same differential voltage. The TCAN334G guarantees ≥ 1.5 V dominant differential into 60 Ω; into 40 Ω the amplitude drops toward the 0.9 V receiver threshold and the margin against noise and ground offset shrinks. It usually still "works" on a short quiet bench bus and fails first in the field. Every node terminated (12 Ω) and the driver cannot reach dominant at all — the bus is dead.
- **Only one end terminated (120 Ω).** Dominant amplitude is fine (higher, in fact). The problem is the *recessive* edge: nothing drives recessive; the terminators pull the pair back together through the bus capacitance. Doubling R doubles that time constant. The datasheet's "highly loaded" column (120 Ω, 200 pF) is exactly this case, and it lists loop delay up to **180 ns** and 5 Mbit received-bit symmetry down to 120 ns — against a 200 ns bit. At 1 Mbit you will not notice. At 5 Mbit, bit-symmetry and the transmitter's own loopback check (§2.5) start failing intermittently. Diagnosable: it works with `bitrate_switch` off and fails with it on.
- **Neither end.** Reflections from open ends; at 45 cm they settle in nanoseconds, so it may even work — right up to the first cable that is a bit longer. Do not.

### 2.4 Bit timing on the STM32G4 FDCAN

FDCAN kernel clock is PCLK1 = **85 MHz** (the firmware's own comment: `tdc_offset = 13; // 13 / 85MHz ~= 152ns`, `fw/moteus.cc:226`). `MakeTime()` (`fw/fdcan.cc:44-72`) picks prescaler 1 and splits the remaining time quanta ≈ 3 : 1 between seg1 and seg2. What the FlexNode firmware therefore programs today:

| Phase | tq per bit | Prescaler | Seg1 | Seg2 | SJW | Sample point |
|---|---|---|---|---|---|---|
| Nominal, 1 Mbit | 85 | 1 | 56 | 28 | 16 | (1+56)/85 = **67 %** |
| Data, 5 Mbit | 17 | 1 | 11 | 5 | 5 | (1+11)/17 = **71 %** |

Both sample points are earlier than usual practice (CiA recommends ≈ 80 % nominal, 75–80 % data; the STM32 examples use 80 %/75 %). Earlier sampling is more tolerant of ringing and less tolerant of propagation and loop delay. On a 45 cm bus ringing is a non-issue and loop delay is the whole budget, so **later is better here**:

| Phase | Recommend | Sample point | Why |
|---|---|---|---|
| Nominal | prescaler 1, seg1 = 67, seg2 = 17, SJW = 17 | 68/85 = **80 %** | Standard; maximises the arbitration loop-delay budget. |
| Data | prescaler 1, seg1 = 12, seg2 = 4, SJW = 4 | 13/17 = **76.5 %** | Pushes the loopback check 12 ns later (§2.5); SJW 4 tq (47 ns) still covers oscillator tolerance between HSI-clocked nodes. |

Both are within the G4 limits `MakeTime` enforces (255/127 nominal, 31/15 data). The mechanism to apply them without touching `MakeTime` already exists — `ApplyRateOverride` (`fw/fdcan.cc:74-89`) — but the `Rate` override struct is not currently exposed as `can.*` config; that is a small firmware addition. **Every node and the corenode must use identical settings.** Clock source caveat: FlexNode has no HSE crystal (`docs/hardware.md`), so the 85 MHz derives from HSI16 at ±1 % over temperature; SJW ≥ 4 tq in the data phase is what absorbs that. **[measure]** with an adapter: error counters at 5 Mbit over a warm-up.

### 2.5 Transceiver reality check: TDC

At 5 Mbit the transmitting controller compares each bit it sends against what it reads back through its own transceiver, at the data sample point. If the transceiver's loop delay exceeds the sample-point position, it reads the *previous* bit and raises a bit error. Transmitter Delay Compensation (TDC) moves that check later by a measured/configured offset.

| | Value | Source |
|---|---|---|
| Data sample point, firmware today | 12 tq / 85 MHz = **141 ns** after bit start | §2.4 |
| TCAN334G loop delay, 60 Ω / 100 pF | 100 ns typ, **135 ns max** | datasheet SLLSEQ7F §5.6 `tPROP(LOOP)` |
| TCAN334G loop delay, 120 Ω / 200 pF | 120 ns typ, **180 ns max** | same |
| MCU pin + controller internal delays | ~10–20 ns | estimate |
| TDC in FlexNode firmware | **disabled** for family 0 | `fw/moteus.cc:224`: `delay_compensation = g_measured_hw_family != 0` |

The moteus comment says family 0 "uses a TCAN334GDCNT, which has a very low loop delay". The datasheet says the margin at the firmware's sample point is **141 − 135 = 6 ns** worst case in a well-terminated bus, and **negative** in a heavily loaded or single-terminated one. Thousands of moteus r4.x boards run 5 Mbit this way, so typical parts clearly clear it — but the design should not rely on typicals.

**Recommendation:** enable TDC for FlexNode — `options.delay_compensation = true`, keep `tdc_offset = 13` (153 ns ≈ seg1 with the §2.4 timing, per ST's guidance of offset ≈ DataTimeSeg1 × prescaler) and `tdc_filter = 2`. It costs nothing, the code path exists and is exercised by other moteus families, and it converts a 6 ns margin into ~150 ns. Two-line change in `fw/moteus.cc`. **[measure]**: with TDC on and off, error counters over 10⁶ frames.

Nothing else about the TCAN334G constrains the design: 5 Mbit is its rated maximum, 45 cm is nothing, and the 100 pF column is where a two-terminator chain of ten sits.

---

## 3. Frame anatomy and cost

### 3.1 Frame time model

CAN FD frame bits, from ISO 11898-1:2015 field lengths:

- **Arbitration rate (1 Mbit):** 11-bit id: SOF 1 + ID 11 + RRS 1 + IDE 1 + FDF 1 + res 1 + BRS 1 = 17 bits up front, plus ACK 1 + ACK-delim 1 + EOF 7 + IFS 3 = 12 bits at the tail → **29 µs**. 29-bit id adds SRR 1 + ID-ext 18 → 36 up front → **48 µs**.
- **Data rate (5 Mbit):** ESI 1 + DLC 4 + 8·N data + CRC field + CRC-delim 1. CRC-17 (N ≤ 16 bytes) with its stuff-count and fixed stuff bits is 27 bits; CRC-21 (N > 16) is 32. So **8N + 33 bits for N ≤ 16, 8N + 38 for N > 16**, at 200 ns each.
- Dynamic bit stuffing in the data adds 0–20 % of the data-phase bits depending on content; it is absorbed in the utilisation margin, not modelled per frame.

**Every command frame is a 29-bit frame**: the host sets bit 15 of the id to request a reply (`lib/python/moteus/transport.py:344`) and the firmware sends any id ≥ 2048 as extended (`fw/fdcan.cc:323-325`). A **reply** has id `(node << 8) | 0`, which is < 2048 for node ids 1–7 (11-bit frame, 19 µs cheaper) and ≥ 2048 for ids 8+. Payloads are rounded up to the next legal DLC — 12, 16, 20, 24, 32, 48, 64 — by `RoundUpDlc` (`fw/fdcan.cc:24-41`).

| Payload (DLC) | 29-bit frame | 11-bit frame |
|---|---|---|
| 8 | 67 µs | 48 µs |
| 12 | 74 µs | 55 µs |
| 16 | 80 µs | 61 µs |
| 20 | 88 µs | 69 µs |
| 24 | 94 µs | 75 µs |
| 32 | 107 µs | 88 µs |
| 48 | 132 µs | 113 µs |
| 64 | 158 µs | 139 µs |

The DLC steps are the whole game for frame design: **33–48 bytes all cost 132 µs; 25–32 all cost 107 µs.** Every frame below is designed to land just under a boundary, and the table says where.

### 3.2 Subframe encoding (stock moteus, `lib/cpp/mjbots/moteus/moteus_multiplex.h`)

Header byte = `0x00` write / `0x10` read / `0x20` reply, `| type<<2` (int8 0, int16 1, int32 2, float 3), `| count` if count ≤ 3, else count 0 and a following count byte (`:646-652`). Then the start register as a varuint (1 byte ≤ `0x7F`, 2 bytes to `0x3FFF`), then the values. Consecutive registers, one subframe.

### 3.3 Arbitration priority — a quirk to know and not rely on

Base id of every corenode command is 0 (bits 28–18 of `0x8000|dest` are zero). Replies from nodes 1–7 are 11-bit frames with base id `node << 8` ≥ 0x100, so **a corenode command always beats them**; replies from nodes 8–10 are extended with base id 0 and a smaller 18-bit extension than the command, so **they beat the corenode**. Consequence: if the corenode streamed ten commands back-to-back, nodes 1–7 could not get a reply in until it paused. The schedule in §5.4 sends with at most two commands outstanding, which sidesteps this entirely. Do not build anything that depends on reply priority.

---

## 4. Node classes and their frames

Aditya: every node has a BLDC axis; some add a servo; some add a servo and a SimpleFOC Mini; some have "other effectors". Four classes cover that; a node's class is whatever its enumerated channel list says (`docs/can-layer.md` §8), not a build-time choice.

Design rules applied to all of them (each a spec change, §12):

- **Fast pair.** Each channel's per-cycle command is `cmd_a, cmd_b` (2 × int16 at `+4`) and its per-cycle readback is `meas_a, meas_b` (2 × int16 at `+7`). `cmd_c` (effort limit) and `meas_c/d` go on the slow lane. Two registers keep the count in the header's low bits — no count byte — so a channel command is 7 B and a readback request 3 B / reply 7 B.
- **Axis fast query is `0x000–0x003` only.** Voltage, temperature and fault (`0x00D–0x00F`) move to the slow lane. Mode (`0x000`) is read every cycle and equals 1 on fault, so fault *detection* latency is unchanged; only the fault *code* waits ≤ 29 ms for the slow lane. This saves 2 B in every command and 5 B in every reply — which is what keeps class C under DLC 32.
- **Mode is written every cycle** (`01 00 0A`). It is 3 bytes, it re-arms the axis, and it is what the stock Python library does; not worth optimising away.

### 4.1 Class A — actuator only

**Command → DLC 16** (14 B, 2 B spare)

| Bytes | Subframe | B |
|---|---|---|
| `01 00 0A` | write int8 ×1 @ `0x000` mode = 10 (position) | 3 |
| `07 20 pp pp vv vv tt tt` | write int16 ×3 @ `0x020` position, velocity, ff-torque | 8 |
| `14 04 00` | read int16 ×4 @ `0x000` mode, position, velocity, torque | 3 |

**Reply → DLC 12** (11 B, 1 B spare)

| Bytes | Subframe | B |
|---|---|---|
| `24 04 00` + 8 | mode, position, velocity, torque | 11 |

Round trip: 80 + 74 = **154 µs** (135 µs if the node id is ≤ 7).

### 4.2 Class B — actuator + one servo (slot 1 = `servo_rc`)

**Command → DLC 24** (21 B, 3 B spare)

| Bytes | Subframe | B |
|---|---|---|
| (class A command) | | 14 |
| `06 94 04 aa aa ss ss` | write int16 ×2 @ `0x214` servo angle, slew | 7 |

No servo readback on the fast lane: a DS3235 has no feedback unless an `enc_i2c` is bound, and its 5 V current (stall detection) is a 35 Hz question. **Reply → DLC 12**, as class A. Round trip **168 µs** (149 µs, id ≤ 7).

Designed under the boundary: 14 + 7 = 21 ≤ 24. Adding a 3-register servo command (with `cmd_c`) would make 23 — still 24. Adding the 3-byte servo readback request would make 24 — still 24, but the reply would go 11 + 7 = 18 → DLC 20 (+14 µs). If a servo *does* have a bound encoder and its angle matters at gait rate, that is the price.

### 4.3 Class C — actuator + servo + SimpleFOC Mini (slot 1 `servo_rc`, slot 2 `bldc_ext`)

**Command → DLC 32** (31 B, 1 B spare — this is the tightest frame in the fleet)

| Bytes | Subframe | B |
|---|---|---|
| (class A command) | | 14 |
| `06 94 04 aa aa ss ss` | servo angle, slew @ `0x214` | 7 |
| `06 A4 04 pp pp vv vv` | write int16 ×2 @ `0x224` Mini position, velocity | 7 |
| `16 A7 04` | read int16 ×2 @ `0x227` Mini position, velocity | 3 |

**Reply → DLC 20** (18 B)

| Bytes | Subframe | B |
|---|---|---|
| `24 04 00` + 8 | axis | 11 |
| `26 A7 04` + 4 | Mini position, velocity | 7 |

Round trip 107 + 88 = **195 µs** (176 µs, id ≤ 7).

This is where the two design rules pay: with the spec's original 3-register commands and 4-register readbacks and `0x00D–0x00F` on the fast lane, this command would be 47 B → DLC 48 (132 µs) and the reply 27 B → DLC 32 (107 µs): **239 µs**, 44 µs more per cycle per shoulder. One more byte in the command as designed and it also jumps to 48 — so **class C has no room for a third effector on the fast lane.** A third effector goes on the slow lane or costs 26 µs.

### 4.4 Class D — actuator + one fast input (foot load cell, or output-side encoder)

Command = class A + `16 97 04` (3 B) = 17 → **DLC 20**. Reply = 11 + 7 = 18 → **DLC 20**. Round trip **176 µs** (157 µs, id ≤ 7). Which nodes, if any, carry a foot load cell is not stated anywhere; class D is here so the budget can absorb it.

---

## 5. The schedule

### 5.1 What rotates and what does not

Aditya asked for round-robin keyed off node id so nothing starves. The answer is that **the fast lane is not a rotation at all: every node is served every cycle, in id order 1→10.** Starvation is impossible by construction. What rotates is the **slow lane**: on cycle *c*, node `(c mod 10) + 1` additionally receives a slow-lane request. Keying it off the id means the schedule is a counter and a modulo, which is exactly right for a purpose-built fleet.

At 350 Hz that is **35 slow-lane visits per second per node**. Whether that is a 35 Hz refresh of *everything* depends on whether a node's slow block fits one 64-byte reply (§5.3).

### 5.2 Fast lane composition, per cycle

Assumed fleet — the counts are Aditya's qualitative statement turned into numbers and are **assumed, not stated**: 2 class C (the two shoulder nodes: GIM 8108-8 + Mini, one with the DS3235), 6 class B (six more servos, one per node, on six actuator nodes — that uses all seven published servos), 2 class A. If "other effectors" add fast channels, they move a node to class D at +22 µs each.

| Class | Count | Cmd / reply DLC | Round trip (id ≥ 8 / id ≤ 7) | Subtotal (all extended) |
|---|---|---|---|---|
| C | 2 | 32 / 20 | 195 / 176 µs | 390 µs |
| B | 6 | 24 / 12 | 168 / 149 µs | 1,008 µs |
| A | 2 | 16 / 12 | 154 / 135 µs | 308 µs |
| **Fast lane** | 10 | | | **1,706 µs** |

Give ids 1–7 to the seven busiest nodes and the fast lane drops to ≈ 1,573 µs. The budget below uses the conservative all-extended figure.

### 5.3 Slow lane

One request+reply pair per cycle, to the rotating node. Content, in one 64-byte reply where it fits:

| Subframe (request) | Reply bytes | What |
|---|---|---|
| `13 0D` | 5 | voltage, temperature, fault |
| `14 08 80 01` (int16 ×8 @ `0x080`) | 20 | caps, status, rail current, profile, build hash, channel count, fleet role, host-loss state |
| `14 06 98 01` (int16 ×6 @ `0x098`) | 16 | IMU accel + gyro (v1 alias) |
| per channel: `16 92 04` (+2..+3) and `17 99 04` (+9..+B) | 8 + 10 | status, mode; `meas_c`, `meas_d`, nonce |

Class A/B/D: 5 + 20 + 16 + 18 = **59 B → DLC 64** ✓ (35 Hz for the whole block). Class C: two channels = 77 B — does not fit. **For class C the slow lane alternates**: even visits carry health + IMU, odd visits carry both channel blocks; each half refreshes at **17.5 Hz**. That is still above the 25 Hz the spec called "ample" for IMU bias tracking only on A/B nodes; for the two shoulders it is below it. If the shoulder IMUs turn out to feed state estimation, promote their IMU read to the fast lane on those two nodes (+4 B request, +16 B reply → command DLC 48 on a class C node: +26 µs each; budgeted in §6 as "next thing that breaks").

Slow request ≈ 20 B → DLC 20 (88 µs); reply DLC 64 (158 µs). **Slow pair = 246 µs per cycle.** Slow-lane *writes* — `cmd_c` effort limits, `0x023–0x029` gains and limits, servo re-arm — ride in the same request frame; they cost reply bytes only if they ask for a reply (they should not).

### 5.4 Cycle timeline at 350 Hz (period 2,857 µs)

The corenode keeps **at most two commands in flight** (send command *k+1* as soon as command *k* has left the TX FIFO, but never command *k+2* until reply *k* has arrived or a 300 µs per-node timeout has expired). This hides node turnaround behind the next command's transmission, guarantees the bus is free for a reply after every second command regardless of the priority quirk in §3.3, and bounds the damage from a dead node to one timeout per cycle.

```
 t (µs)   bus
    0     ┌─ cmd 1 (C, 107) ─┐┌─ cmd 2 (C, 107) ─┐┌ reply 1 (88) ┐┌─ cmd 3 (B, 94) ─┐┌ reply 2 (88)┐
  ~480    ┌─ cmd 4 (B) ─┐┌ reply 3 (74) ┐┌─ cmd 5 (B) ─┐┌ reply 4 ┐┌─ cmd 6 (B) ─┐┌ reply 5 ┐ …
 ~1540    … ┌─ cmd 10 (A, 80) ─┐┌ reply 9 ┐┌ reply 10 (74) ┐
 ~1710    ┌─ slow req → node (c mod 10)+1 (88) ─┐┌──────── slow reply, DLC 64 (158) ────────┐
 ~1950    ┌ FaF pixel (67) ┐┌ FaF servo-arm (67) ┐  … up to a cap of 3 frames / 250 µs
 ~2200    ·········· idle slack ≈ 650 µs (retransmits, error frames, stuffing, turnaround jitter) ··········
 2857     next cycle
```

Where each thing lands:

- **Fast lane, 0 → ~1,710 µs.** Commands in id order; replies interleave. Reply *k* arrives after node *k*'s turnaround, which is main-loop latency (`fw/moteus.cc:348-353` polls CAN RX from the `for(;;)` loop) — **[measure]**, unmeasured, probably tens of µs, hidden by the two-in-flight rule as long as it is under one frame time.
- **Slow lane, ~1,710 → ~1,960 µs.** One pair.
- **Fire-and-forget, ~1,960 → ~2,200 µs.** Pixel colour, tare, servo re-arm, named-posture broadcast: drained from a queue with a per-cycle cap (3 frames or 250 µs) so they can never push the next cycle. Fire-and-forget frames have no reply bit, so they are 11-bit frames (48 + data) — cheap.
- **Slack, ~2,200 → 2,857 µs.** ≈ 23 % of the period. This is where automatic retransmission (`fw/moteus.cc:217`, on) and error frames go without stealing from the next cycle.

The corenode's own FDCAN has, on the G4, a fixed message RAM of **3 TX FIFO elements and 3 + 3 RX FIFO elements** (RM0440; not re-verified in this repo). Ten replies arrive ~75–110 µs apart; the corenode must service RX from an interrupt, not a main loop, or the fourth reply overwrites the first. Note it in the corenode firmware requirements.

### 5.5 Operating points

Cycle cost 1,952 µs (all-extended, assumed mix). Utilisation = cost × rate.

| Loop rate | Period | Bus utilisation | Slack per cycle | Verdict |
|---|---|---|---|---|
| 200 Hz | 5,000 µs | 39 % | 3,048 µs | Bring-up. Room for verbose slow lane, tunnel traffic, debugging. |
| 250 Hz | 4,000 µs | 49 % | 2,048 µs | Comfortable. |
| 300 Hz | 3,333 µs | 59 % | 1,381 µs | **First walking target.** Under the 65 % rule with margin for class-D additions. |
| **350 Hz** | 2,857 µs | **68 %** | 905 µs | **Design operating point.** Just over the 65 % rule; acceptable because the slack is still 4–5 frames wide. |
| 400 Hz | 2,500 µs | 78 % | 548 µs | Ceiling for a single chain. One retransmit storm eats the slack. |
| 450 Hz | 2,222 µs | 88 % | 270 µs | Not viable — slow lane and fire-and-forget starve. |
| 500 Hz | 2,000 µs | 98 % | 48 µs | Not viable. |

With ids 1–7 on the busiest nodes (1,819 µs): 350 Hz → 64 %, 400 Hz → 73 %.

**Recommended: 1 Mbit / 5 Mbit (unchanged), 350 Hz loop, ≈ 68 % utilisation, ≈ 0.9 ms slack.** Bring up at 200 Hz, walk at 300 Hz, tune toward 350 Hz. Nothing in the servo (50–330 Hz devices) or policy path benefits from more, and the FOC loop is local at 30 kHz regardless.

---

## 6. What breaks first, and the upgrade path

In the order it will happen:

1. **A class C node grows one byte.** 31 → 32 is fine; 33 is DLC 48 and +26 µs. Adding a fast IMU read to a shoulder, a third effector, or per-cycle stiffness (§7.2) does it. Two shoulders → +52 µs/cycle → 350 Hz goes from 68 % to 70 %. Survivable once; not twice.
2. **Fast input channels spread.** Each node promoted to class D is +22 µs. Four foot load cells → +88 µs → 71 %.
3. **Node count.** Each added class A node is +154 µs; a class B, +168 µs. At 12 nodes the 350 Hz point is ≈ 80 %. At 13 (the old assumption) it is ≈ 86 % — which is why the old spec's 325 Hz / 65 % line was already optimistic.
4. **Slow-lane refresh** on class C is already at 17.5 Hz per half; a fourth channel on a node pushes it to three-way alternation.
5. **Corenode RX servicing** (§5.4) — a software problem, not a bus one, but it will look like a bus one.

**Upgrade path: second chain on FDCAN2.** Split 5/5 by side (or front/rear). Each chain's cycle is ≈ 0.95 ms; at 350 Hz that is ≈ 33 %, and 600 Hz becomes reachable at ≈ 57 %. Termination: each chain gets its own two ends (corenode end + last node). The corenode runs two identical schedulers offset by half a period so its RX interrupt load is even. The FlexNodes need nothing — `can.prefix` can namespace the two chains if they are ever bridged, but physically separate buses do not need it. This is also the fault-containment split the site already promises ("one bus fault takes down one side"). Not needed at 10 nodes; the design is written so that enabling it is a corenode change only.

---

## 7. Command and telemetry semantics

### 7.1 Who computes what

- **Jetson** runs the policy. Its output should be **per-joint targets** — position, velocity and feed-forward torque for each of the ten axes, plus servo angles and Mini positions — at policy rate. Whether the policy emits joint space directly or foot coordinates that something converts through IK is a Jetson/corenode software decision and is **not stated**; the bus does not care, but the corenode must receive joint-space targets at least as often as it wants to change them, and it interpolates (or holds) between policy updates at bus rate.
- **Corenode** owns the schedule, packs frames from templates (§7.4), unpacks replies into a fleet-state struct, hands the Jetson a consolidated state at whatever rate the Jetson wants, and runs the failure policy (§9) without the Jetson.
- **FlexNode** runs FOC at 30 kHz on the axis, the servo pulse and the Mini commutation locally, and answers what it is asked.

### 7.2 Registers, by lane

| Lane | Direction | Registers | Notes |
|---|---|---|---|
| Fast, every cycle | cmd | `0x000` mode = 10; `0x020` position, `0x021` velocity, `0x022` ff-torque | stock (`fw/moteus_controller.cc:236,270-272`) |
| Fast | cmd | `0x2n4` `cmd_a`, `0x2n5` `cmd_b` per channel | servo: angle, slew · Mini: position, velocity |
| Fast | tel | `0x000–0x003` mode, position, velocity, torque | mode = 1 flags a fault immediately |
| Fast | tel | `0x2n7` `meas_a`, `0x2n8` `meas_b` for Mini (and class-D inputs) | Mini: position, velocity · load cell: force, **contact** · encoder: angle, rate |
| Slow, 35 Hz (17.5 Hz on C) | cmd | `0x023` kp scale, `0x024` kd scale, `0x025` max torque, `0x028` velocity limit, `0x029` accel limit; `0x2n6` `cmd_c` effort limits; `0x2n3` channel mode | stock `0x023–0x02A` (`:273-280`). Written only when changed. |
| Slow | tel | `0x00D–0x00F`; `0x080–0x087`; `0x098–0x09D` IMU; per channel `+2,+3,+9,+A,+B` | see §5.3 |
| Fire-and-forget | cmd | `0x0B0–0x0B4` pixel; tare; servo re-arm; named posture (broadcast `0x7f`) | no reply bit, 11-bit frames |
| Boot only | both | `0x100`, `0x150–0x153`, `0x0FF`, `0x0C0–0x0C3` name, channel `type/caps` | `docs/can-layer.md` §8 |

**Per-cycle stiffness modulation** (`0x023–0x025` every cycle, as some policies want) costs +8 B in every command: class A 22 → DLC 24 (+14 µs), class B 29 → DLC 32 (+13 µs), **class C 39 → DLC 48 (+26 µs)**. Fleet cost ≈ +150 µs/cycle → 350 Hz at 73 %. Affordable on a single chain only if nothing else grows; free on two chains. Decide when the policy exists.

### 7.3 Servo semantics on the wire

`servo_rc` `cmd_a` is the target angle in 0.05° (int16), `cmd_b` the slew limit in 0.5 °/s (resolution table in `docs/can-layer.md` §3.5). The node runs the pulse train and slew limiting; if an `enc_i2c` is bound it closes the outer loop locally. The corenode writes the target every cycle whether or not it changed — that is the servo channel's watchdog refresh (§9). `cmd_c` (5 V current limit for stall cut-off) is slow-lane, once.

### 7.4 Frame templates — how the corenode composes a multi-effector frame

At enumeration the corenode builds, per node, a **byte template**: the fixed subframe headers with the value slots left blank, plus a table of (offset, source-field, scale). Per cycle, packing is a `memcpy` of the template and int16 stores at fixed offsets; no varuint encoding or subframe logic runs in the loop. For the class C node in §4.3 the template is 31 bytes with value slots at offsets 2 (mode), 5–10 (axis), 17–20 (servo), 24–27 (Mini). Unpacking uses the same idea in reverse against the expected reply layout; a reply whose subframe headers do not match the template is a protocol error, logged with the node's name (`0x0C0–0x0C3`) and dropped. Templates change only when the channel list changes — i.e. never at runtime, which is the whole point of "hardware changes need a reflash".

---

## 8. Node identity in this topology

Unchanged from `docs/can-layer.md` §4.1: CAN id (1–10, address), UUID (`0x150–0x153`), name (`0x0C0–0x0C3`). The corenode holds the fleet manifest — ten rows of (id, UUID, name, expected class, expected profile/build hash) — and refuses to start the schedule on any mismatch. Give **ids 1–7 to the two class C and five of the class B nodes** (cheapest replies), 8–10 to the rest. Numbering by id in physical chain order is tidy but not required.

---

## 9. Failure policy with the corenode as the only master

`docs/can-layer.md` §7 was written for a Jetson-or-brainstem master. With the corenode as the sole master three things change: the beacon source is fixed, the fast lane *is* a heartbeat, and the question G2 answers becomes "what if the corenode dies" — a different, rarer, event.

### 9.1 What a FlexNode does when corenode frames stop

**Detection.** Stock: `servo.default_timeout_s` (`fw/bldc_servo_structs.h:612`, default **0.1 s**) is reset by every command frame. At 350 Hz a frame arrives every 2.9 ms, so 100 ms is 35 consecutive missed cycles. **Recommend 30 ms** (≈ 10 cycles): long enough that a retransmit burst or a single dropped reply never trips it, short enough that a fallen corenode is noticed within a stride. Config, not code; can be tuned per node. **[measure]** false-trip rate at 30 ms once the bus is real.

**Axis safe state.** On timeout the axis enters `timeout_mode` (`:622`, default **12 = kZeroVelocity**): a velocity-zero servo loop — it does not hold position, it *damps* motion, with torque capped at `timeout_max_torque_Nm` (`:613`, default **5 Nm**). For a 4.8 kg quadruped that is the right primitive: the robot sags to the ground against damping rather than collapsing or locking. Recommend: hips/shoulders (backdrivable QDD) keep mode 12 with the cap tuned to roughly body-weight support so the sag is slow; knees (14:1 belt, less backdrivable) the same, and evaluate mode 15 (brake) only if the belt turns out to overrun.

**Channel safe state**, per type — new firmware, since channels do not exist yet:

| Type | On timeout | Why |
|---|---|---|
| `servo_rc` | hold last target for `ch.hold_ms` (recommend 200 ms), then `mode = 0` (pulse stops, servo goes limp) | An instantly limp spine/tail drops the torso; a stale target held forever fights the sagging legs. A short hold then release is the compromise. |
| `bldc_ext` (Mini) | `mode = 0` immediately: driver disabled, coast | Open-loop commutation with a stale target is worse than coasting. |
| inputs (`loadcell`, `enc_i2c`, `imu`, `tof`) | keep sampling | Nothing to make safe; data is wanted when the corenode returns. |
| `pixel` | fault pattern on pixel 0 | Human-visible "I have lost my master". |

Recovery is automatic: the next valid command frame re-arms the axis (mode write) and the corenode re-arms channels (`mode = 1`) on its first slow-lane visit.

### 9.2 What the corenode does when a FlexNode stops replying

A node that misses its 300 µs reply window is marked stale for that cycle; three consecutive misses mark it *lost*. The corenode keeps commanding it (the frame costs the same either way and re-arms it the instant it is back), tells the Jetson which limb is degraded, and applies the corenode-level fallback: a **standing posture on the remaining nine** with the affected limb's neighbours commanded to hold. This is the "brainstem still has authority to put the robot into a safe pose" from the site, made concrete. It needs no bus feature at all.

### 9.3 G0 / G1 / G2, reconciled

- **G0 — per-node timeout.** As above. Always on. Now with a 30 ms recommendation and per-channel safe states.
- **G1 — presence beacon.** The corenode is the only possible source. But **the corenode already sends every node a frame every 2.9 ms; the fast lane is the beacon.** A separate 10 Hz broadcast adds nothing for liveness. What survives of G1 is the *fleet-state* half: a fire-and-forget broadcast write to `0x086` (`0x7f` destination, already accepted by every node — `fw/moteus.cc:307,319`) carrying a named posture or a "degrade" flag, so the corenode can put all ten nodes into a posture with one frame when the *Jetson* dies. That is the case G1 should be redefined around: **G1 = corenode-driven degraded mode on Jetson loss**, not node-side beacon tracking. Register `0x087` (host-loss state) stays, driven by the node's own G0 timer.
- **G2 — deputy.** With one master, G2 protects against *corenode* death. **Recommendation: do not build it for CATBOT.** The corenode is a G4 running fixed-rate code with a hardware watchdog and no OS — the "boringly predictable" tier by design; its failure is orders of magnitude rarer than the Jetson's, and when it happens G0 already lands the robot in a damped sag within 30 ms, which is a defensible outcome for a 4.8 kg machine. Against that, G2 adds the only unsolicited transmitter on the bus, a fold sequence executed blind by a node that cannot see its peers, and the failure modes listed in the previous report (a wrong deputy is indistinguishable from the host). The one thing worth taking from G2 is a **corenode hardware watchdog that, on reset, immediately broadcasts a "hold" posture** — which is G1's fleet-state frame, sent by the recovering master. Keep §7's G2 text in the spec as a documented, disabled option for a future multi-master fleet; delete `deputy_rank` from the CATBOT config.

### 9.4 Jetson loss

Not a bus event. The corenode notices its Jetson link is silent (its own watchdog, threshold a Jetson-side question — a few hundred ms), stops interpolating toward stale targets, and drives the fleet to a standing or crouched posture from its own table via the fast lane it is already running. The bus schedule does not change at all. This is the failure that will actually happen, and it is handled entirely above the CAN layer.

---

## 10. Bring-up order for this design

Each step is testable with the adapter and no motor.

1. **Adapter.** Everything below is blind.
2. One node: `moteus_tool --info`, read `0x0FF`, `0x080`, name; measure **node turnaround** (command → reply, scope on CANH). **[measure]** — the number that decides whether §5.4's two-in-flight rule hides it.
3. Error counters at 5 Mbit, TDC off vs on (§2.5); adopt the §2.4 sample points via a `can.*` override.
4. Two nodes, ids 1 and 8: confirm the 11-bit/29-bit reply split and the arbitration behaviour of §3.3 on a scope.
5. Class A schedule at 200 Hz from the adapter host: measure real cycle time against 1.95 ms; fit the stuffing overhead.
6. Channel infrastructure (`docs/can-layer.md` §11 steps 4–7) → class B and C frames; confirm DLC 24 / 32 on the wire.
7. G0 at 30 ms: pull the adapter mid-schedule, scope the time to mode 12.
8. Corenode firmware: templates, two-in-flight, RX in interrupt, slow-lane rotation, fire-and-forget cap.
9. Ten nodes at 300 Hz; then 350 Hz; read utilisation from the corenode's own busy-time counter.

Hard gates unchanged: AS5047 soldered before any position work; phase order validated on a current-limited supply before calibration or sustained drive.

---

## 11. Open questions and what each blocks

| Question | Blocks | Level |
|---|---|---|
| **Node turnaround latency** (main-loop polled) | Whether two-in-flight hides it; if > ~100 µs the cycle grows by up to 10 × the excess | **[measure]** |
| **SimpleFOC Mini attachment** (own node / TIM1 on SPI2 pads / v1.1) — unchanged from the previous report | Class C exists at all | Design |
| **Jetson ↔ corenode link** (USB, UART, SPI?) and its rate | §7.1 interpolation; Jetson-loss detection threshold | Design |
| Which nodes carry foot load cells / output encoders | Class D count; +22 µs each | Implementation |
| Does the policy need per-cycle stiffness (`0x023–0x025`)? | +150 µs/cycle; class C → DLC 48 | Implementation |
| Exact effector mix per node (which six actuator nodes host the six non-shoulder servos) | Nothing on the protocol; the manifest | Implementation |
| Whether a separate G0 exists for acoustic wake | Power architecture only; the corenode must be FDCAN-capable regardless | Design (not bus) |

---

## 12. Changes this design requires in `docs/can-layer.md`

For the coordinator to apply; I have not edited `docs/`.

| § | Change |
|---|---|
| 0, scope note | Fleet is **10 nodes**, all with axis 0. Delete the "10 + 3 non-actuator" assumption; `aux` and `sense` profiles are not used on CATBOT. |
| 1, L0 | State that command frames are 29-bit (reply bit) and replies are 11-bit for ids ≤ 7. Add the §3.3 priority note. |
| 3.3 | Define the **fast pair**: per-cycle command = `cmd_a, cmd_b`; per-cycle readback = `meas_a, meas_b`; `cmd_c`, `meas_c/d` are slow-lane. |
| 3.4 | Re-bind so the fast pair is the loop-critical pair: `servo_rc` `meas_b` = 5 V current; `loadcell` `meas_b` = **contact flag**, `meas_d` = force rate. |
| 5.1 | Fast axis query is `0x000–0x003` only; `0x00D–0x00F` moves to the slow lane. Slow lane alternates two blocks on nodes whose block exceeds 64 B. Add the fire-and-forget per-cycle cap and the two-in-flight rule. |
| 5.2 | Replace the frame model with §3.1 (29-bit arbitration cost, CRC-17/21 split, DLC quantisation) and the table in §3.1. |
| 5.3 | Replace the budget table with §5.5; operating point 350 Hz / 68 % on one chain of 10. |
| 6 | Profiles are off the CATBOT path; `full` is the CATBOT image. |
| 7 | G1 = corenode-driven degraded mode on Jetson loss, fleet-state broadcast only; G2 documented but **not built**; add per-channel timeout behaviours and the 30 ms `default_timeout_s` recommendation. |
| 9 (new 9.5) | Physical layer: §2 of this document — sample points, **enable TDC for family 0**, termination rules, chain-not-star rationale. |
| 11 | Build order per §10; add turnaround measurement as step 2. |
| 12 | Close "10 vs 13"; close "brainstem G0 or G4" as "the corenode must be FDCAN-capable"; add the Jetson-link and turnaround questions. |
