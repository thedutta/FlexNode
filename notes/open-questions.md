# Open questions

Decisions not yet made, with the current recommendation. Edit in place when one closes.

## 1. Flash budget (the constraint that shapes everything)
~16 KiB free at `ef0d948`. Still to add: servo wrappers (0x0B8–0x0BB), load cell + on-node contact flag (0x088–0x08B), SimpleFOC relay, ToF summary registers. Each is small (1–3 KiB) but there is no margin.

Recommendation, in order:
1. **Reclaim** the unused encoder drivers from the aux port (BiSS-C, iC-PZ, MA732, AKSIM2, MA600, CUI AMT21/22, Orbis, AS5048, sine/cosine, quadrature, hall). FlexNode uses only the onboard AS5047 (SPI) and the AS5600L (I²C). Off the control path, so low risk. **Measure from the linker map first**; cut with numbers.
2. Keep **one image**. Capabilities register 0x080 already lets the host discover per-node roles; config differentiates nodes.
3. Per-role images only if a role genuinely won't fit. Stripping FOC for pure I/O nodes would free the most but is deep surgery in `MoteusController`.
4. `led.imu_demo` (float HSV) is the first thing to drop if space runs out.

Aditya's stance: "17 kB is way too less"; open to trimming, dynamic per-role flashing, or both.

## 2. SimpleFOC Mini topology (blocks the relay design)
CATBOT will have SimpleFOC Minis "to control and relay over CAN depending on config". Unanswered:
- Are the Minis **peripherals of a FlexNode** (driven over its aux port: UART Commander / Step-Dir / PWM), with FlexNode exposing relay-target registers? Small, clean.
- Or **their own CAN nodes**? Then coexistence matters: SimpleFOC CAN is classic CAN 2.0 and a classic-only node errors on CAN-FD frames. They cannot share the FlexNode bus unless they also do FD or sit on a separate bus.
- Classic CAN or CAN-FD?

Ask before designing the relay layer.

## 3. CAN adapter
None on the bench. Everything after the LED and IMU is blind without one. First CAN session: `moteus_tool --info`, read 0x080 and 0x0FF, `bus_V` vs DMM, write 0x0B1=255 and watch the LED go red.

## 4. Encoder
AS5047 not soldered. Until it is: no calibration, no position. Encoder bring-up on PC6 is its own gate.

## 5. IMU details deferred
Full-scale and ODR are fixed in firmware (±4 g, ±500 dps, 104 Hz). No re-init after a bus fault. No orientation calibration. Fine for now; revisit when fusion starts.

## 6. LED extras deferred
Breathe/chase modes (0x0B0 = 2/3) are reserved and currently treated as solid. Per-pixel CAN writes exist as an API (`SetPixel`) but have no register yet.

## 7. Portfolio / site
Bench photos: one first-light photo is live. More can go in as milestones land. The subagent handles it; give it facts, never let it invent images or numbers.
