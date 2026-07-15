# FlexNode — Hardware

Status: **v1.0, as-ordered.** Not yet assembled or validated. Values below reflect the schematic/BOM committed to fab.

## Block diagram

```
        ┌──────────────── CAN-FD bus (daisy-chain) ────────────────┐
        │                                                          │
   [J3 in]──┬──[TCAN334G]──FDCAN──┐                          [J4 out]
            │   (120Ω term @      │                                
       120Ω bridge   leaf nodes)  │                                
                                  │                                
   V+ (10S) ─┬─[LGS5160C buck]─5V─┼─[MCP1700]─3V3───────┐          
             │                    │                     │          
             │              ┌─────┴─────┐          ┌────┴─────┐    
             │              │ STM32G4   │──SPI────│ AS5047P   │    
             │              │  (FOC +   │          │ encoder   │    
             │              │  fusion)  │──I²C─┬──[LSM6DS3 IMU]│    
             │              └─────┬─────┘      └──[VL53L7CX ToF]    
             │                    │ SPI                            
             │              ┌─────┴──────┐                         
             └──────────────│ DRV8353S   │──gate──[6× BSC016N06NS]─── 3-phase motor
                            └────────────┘         ↑ shunts → current sense
                                                                    
   PC13 ── servo/LED out ── [50 mΩ shunt] ── 5 V current sense (PB11)
   PF0  ── WS2812B status LED
```

## Power tree

| Stage | Device | In → Out | Notes |
|---|---|---|---|
| Motor bus | — | battery V+ | 10S-class, feeds FET bridge + DRV VM directly |
| Aux buck | LGS5160C | V+ → 5 V | 65 V-rated sync buck, ~426 kHz, internal comp, hiccup SCP/OCP, EN tied to V+ |
| Logic LDO | MCP1700 | 5 V → 3.3 V | MCU + sensor rail |
| Buck output cap | 3× 10 µF (25 µF eff.) | — | transient-driven; feed-forward Cff optional/DNP |

The buck EN is tied directly to V+ (no UVLO divider). Feedback: R15 (1 MΩ) / R17 (249 kΩ), RFREQ = 200 kΩ, PG pull-up R16 (100 kΩ). See the LGS5160C notes in the project datasheet folder for the full passive spec.

## Motor stage

- **FETs:** 6× BSC016N06NS (60 V / 1.6 mΩ, TDSON-8). Chosen over moteus's TPH1R204PL for the higher voltage headroom at 10S; footprint changed to TDSON-8 to match.
- **Gate drive:** DRV8353S, 3-PWM mode (low-side inputs tied to `MOTOR_HIZ`), 7.5 Ω gate resistors + BAT41 clamp diodes as in moteus. Gate-drive currents inherited from the r4.11 (`hw_version = 8`) register set — functional for the BSC016N06NS, not separately re-tuned.
- **Current sense:** three in-line shunts → DRV current-sense amps (SOA/SOB/SOC) → RC filter → MCU ADC (PB0/PB1/PB2).
- **Copper:** 4-layer, 1 oz all layers. Validated against CATBOT's real phase currents (nominal ~2–5 A, transient peaks to ~25 A stall); the actuator thermally limits well before the board does, with comfortable FET margin. 1 oz also allows the tighter JLCPCB clearance that 2 oz would forbid.

## Rotor feedback

- **AS5047P** on-axis magnetic encoder, SPI. **Chip-select moved to PC6** (moteus used PB11). Mounted on the **back layer** (single-sided assembly build).

## Communications

- Single **TCAN334G** CAN-FD transceiver (moteus's optional second transceiver footprint is staggered/DNP and left unpopulated).
- **Daisy-chain**: connectors J3 (in) and J4 (out) share the differential pair, so nodes chain node-to-node on one bus.
- **Termination**: a 120 Ω resistor selectable by a back-layer bridge pad — closed only on the two **leaf (end) nodes** of the chain; open (unterminated) on all interior nodes. Do **not** terminate every node.
- **Protection**: SZNUP2105 dual TVS across the pair; common-mode choke on the transceiver side.

## Sensors (I²C1, PB8/PB9, 2 kΩ pull-ups)

| Sensor | Part | Function | Notes |
|---|---|---|---|
| IMU | LSM6DS3TR-C | 6-axis accel+gyro | CS→3V3 (I²C mode), SA0→GND (addr 0x6A) |
| ToF | VL53L7CX | 8×8 multizone ranging | INT on PB10 |
| Aux/ABS | (external, J2) | shared I²C for an off-board absolute encoder | same bus, distinct address |

`PB8` doubles as BOOT0 — as on moteus, the `nBOOT0` option byte must be set to **boot-from-flash** so the I²C use of the pin doesn't drop the board into the ROM bootloader at reset.

## Auxiliary I/O

- **Dedicated I²C port** (J2) on the main I²C1 bus — brings out SCL/SDA/3V3/GND for a secondary/absolute encoder or any off-board I²C peripheral, sharing the bus with the onboard IMU and ToF (distinct addresses).
- **SPI expansion pads** — the SPI bus is exposed on pads, with spare port GPIOs available as chip-selects, so extra SPI peripherals can be added without reworking the core nets.
- **Connectors**: JST-PH or direct-solder throughout (no XT90-class connectors — no board space for them on CATBOT).
- **Servo / Aux port** on `PC13` (J1) — servo pulse or LED. PC13 is an RTC-domain GPIO (weak, ~3 mA, slow slew) — fine for a 50 Hz servo pulse or a few LEDs; buffer it for long/fast strings. Electrically isolated from the FOC peripherals, so it can't perturb motor control.
- **5 V current sense** on `PB11`: 50 mΩ high-side shunt (R11) → 10 k/10 k divider → ADC. Coarse (~32 mA/LSB scale, assumes a stable 5 V rail); calibrate the zero-offset in firmware. Adequate for "is the servo stalling" monitoring.
- **WS2812B** status LED on `PF0` (free because no external HSE crystal is used). Drive via SPI/timer-DMA to keep the 800 kHz stream off the FOC ISR.

## Key pin remaps vs moteus r4.11

| Signal | moteus | FlexNode |
|---|---|---|
| AS5047 CS | PB11 | **PC6** |
| 5 V current sense | — | **PB11** |
| Servo / LED | PC13 (2nd-enc CS) | **PC13** (servo/LED) |
| Motor PWM phase A / C | PA0 / PA2 | **PA2 / PA0** (swapped — see firmware fix) |
| Current sense phase A/B/C | PB0 / PB1 / PB2 | PB0 / PB1 / PB2 (unchanged copper) |
| ToF INT | — | PB10 |
| WS2812 data | LED (PF0) | PF0 |

Because PC6 (a hardware-version strap) is repurposed, the on-board version detection is no longer valid — hence the firmware `hw_version` hardcode. See [`firmware.md`](firmware.md).
