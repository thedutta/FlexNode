# Bench setup

What is on the bench as of 2026-09-06 and the rules that were learned the hard way.

## Power
- **13 V bench adapter** into the motor bus input. Not the 9 V battery: the DRV8353S needs VM ≥ 9 V and an alkaline sags under load, giving phantom driver-undervoltage faults.
- Aux rails measured: 5 V buck and 3.3 V logic both stable from the adapter. Quiescent current is tens of mA.
- The 3.3 V pin on the FLASH header is the board's own rail. **Do not connect it to a USB-stick ST-Link's 3V3 pin** (that pin is an output on the clones; two regulators would fight). Leave it open.

## SWD header
FlexNode FLASH header, in order: **NRST · CLK · DIO · 3V3 · GND**.

| FlexNode | ST-Link V2 |
|---|---|
| GND | GND (connect first, disconnect last) |
| DIO | SWDIO |
| CLK | SWCLK |
| NRST | RST — see the rule below |
| 3V3 | open |

Reseating error that bit us: shifting a wire one pin over. CLK↔SWCLK and DIO↔SWDIO must not be swapped or offset.

> Gotcha, the NRST rule. A USB-stick ST-Link clone that is **not powered** (USB unplugged) drags NRST low and holds the STM32 in reset. Symptom: board dark on "standalone" power-up, springs to life the instant the ST-Link is plugged into the laptop. Conversely, with the ST-Link **powered**, NRST wired is good: it helps openocd connect, and pulling it made `init mode failed (unable to connect)` appear.
> Rule: keep NRST wired while the ST-Link is on the laptop. To run standalone, unplug the ST-Link entirely (pulling just GND also works, it floats the whole connector). Don't leave a dead ST-Link hanging off NRST.

Pulling any single jumper mid-session tends to unseat a neighbour. After touching the harness, run `probe` before anything else.

## ST-Link driver (Windows)
- Adapter: ST-Link V2 clone, VID 0483 / PID 3748. Windows binds nothing by default (Problem code 28).
- Bound to **WinUSB via Zadig** (Options → List All Devices → "STM32 STLink" → WinUSB → Install). Needs an admin prompt, so Aditya does that click. Zadig 2.9 exe was left at `Dev\flash\zadig-2.9.exe`.
- OpenOCD: xPack 0.12 installed with `winget install xpack-dev-tools.openocd-xpack`. The script finds `openocd.exe` under `%LOCALAPPDATA%\Microsoft\WinGet\Packages\xpack-dev-tools.openocd-xpack*`.
- "Target voltage: 2.2–2.3 V" in the openocd banner is the ST-Link reading its own floating VTref pin. Meaningless here; the real rail is 3.3 V.

## Flashing vs running
- Flashing with the ST-Link powered and all four wires in: fine, and the `reset` at the end of the flash **does** restart the board into the new image (verified 2026-09-06 by watching the new behaviour appear immediately after a flash).
- Standalone test = adapter power only, ST-Link fully disconnected. Watch the **first 3 seconds** after power-up: the status LED does one blue breath then goes dark, so a late glance sees a dark LED and learns nothing.
- Unplugging power **mid-flash** leaves a partially erased app region and the board won't boot. Not harmful; reconnect and reflash. The vector table and bootloader (first two images) survive if they had already verified.

## Not on the bench yet
- No CAN-FD adapter (no fdcanusb). Everything so far has been verified through SWD reads and the LED.
- No motor connected. No AS5047 soldered (back side). Power stage never energised.
