# Firmware map

Where the FlexNode changes live in the moteus fork, and how to extend them. Upstream moteus structure is described in `moteus-r4-parent/CLAUDE.md`; this covers only what we added or changed.

## Identity and pins: `fw/moteus_hw.cc`
- Board hardcoded `{family 0, hw_version 8, hw_pins 4}` (r4.11-class, selects the DRV8353 register path). Strap autodetect deleted. FlexNode is permanently family 0; n1/c1/x1 maps are gone.
- **Phase fix (drive side)**: `pwm1 = PA_2_ALT0`, `pwm3 = PA_0_ALT0`. Netlist-verified 2026-07-21. Sense side stock (PB0/PB1/PB2). Schematic labels CUR1/CUR3 are swapped vs copper: trust pins, not labels. **Not yet validated on a motor.**
- `vsense_adc_scale = 0.067944` encodes the as-built R30 = 1.2 kΩ divider (stock 0.017947). DMM-verify `bus_V` at first CAN contact.
- `as5047_cs = PC_6`. `debug_led1 = NC` (PF0 now belongs to the WS2812), `ws2812 = PF_0` (new field in `MoteusHwPins`).

## Status LED: `fw/ws2812_led.{h,cc}`
See [`ws2812-led.md`](ws2812-led.md). Member `led_` of `MoteusController::Impl`, polled from `Impl::PollMillisecond()`.

## IMU: aux2 I²C device type
I²C1 on PB8/PB9 is, in moteus terms, **aux2's I²C bus**, so the IMU rides the existing engine rather than owning a second one (the future AS5600L joint encoder needs the same bus and the Encoder-2 path).
- `fw/aux_common.h`: `I2C::DeviceConfig::Type` gained `kLsm6ds3` and `kBoardDefault` (the new default for `type`); `I2C::Status` gained `ImuStatus imu` (raw ax/ay/az gx/gy/gz temp, nonce, whoami, error_count, active). Telemetry: `aux2.i2c.imu.*`.
- `fw/aux_port.h`: `AuxPort` takes an `I2cDefault`. `kDefaultOnboardLsm6ds3` resolves `aux2.i2c.devices.0` from `kBoardDefault` to the IMU at `0x6A`, 10 ms poll, and claims aux2 pins 0/1 as I²C. Init = one burst write CTRL1_XL/CTRL2_G/CTRL3_C (`0x48/0x44/0x44`: 104 Hz, ±4 g, ±500 dps, BDU+IF_INC), then WHO_AM_I (expect `0x6A`), then 14-byte bursts from `OUT_TEMP_L`. Raw buffer widened to 14 bytes.
- `fw/moteus_controller.cc`: aux2 is constructed with `kDefaultUartDisabled` (PB8/PB9 is the sensor bus, not a debug UART) and `kDefaultOnboardLsm6ds3`.
- Disable per node with `conf set aux2.i2c.devices.0.type 0`.
- Not yet: re-init after a bus fault (stays `active=false` with `error_count` climbing until reboot).

## FlexNode register block: `fw/moteus_controller.cc`
`enum class Register` gained the `0x080–0x0FF` block per `docs/can-layer.md` §5. Implemented: `0x080` capabilities (bit2 IMU when WHO_AM_I answers, bit4 pixel), `0x081` status, `0x098–0x09F` IMU (accel g / gyro dps / temp / nonce, moteus int8/int16/int32/float scalings), `0x0B0–0x0B4` pixel mode/RGB/brightness, `0x0FF` block version = 1. **Unverified over CAN** (no adapter).

### To add a register
1. Add the enum value in the `// === FlexNode register block ===` section.
2. Add a `case` in **both** `Read()` and `Write()`; the switches are exhaustive under `-Werror`. Read-only registers go in the `// Not writeable` group in `Write()`.
3. Scale with the existing helpers: `IntMapping` for integers, `ScaleMapping(v, int8_scale, int16_scale, int32_scale, type)` for physical units, `ScaleTemperature` for °C. Values > 127 need int16 on the wire.
4. Update the table in `docs/can-layer.md` and bump `kFlexNodeBlockVersion` if the layout changes.

### To add an aux I²C device type
1. `aux_common.h`: enum value; status fields if it doesn't fit `DeviceStatus`.
2. `aux_port.h`: cases in the three switches over `DeviceConfig::Type` (init write, poll start, parse), plus register constants next to the AS5600 ones. All three switches are exhaustive.
3. If it should be on by default on FlexNode, extend `I2cDefault` and the resolution block in `HandleConfigUpdate()`.

### To add a config group
`persistent_config->Register("name", &struct_, callback)` before `persistent_config.Load()` in `moteus.cc` (the controller's constructor runs before Load, so anything registered there is fine). Serialize with `MJ_NVP`. Integers as `int32_t`; that's what `conf set` handles cleanly.

## Bench tooling: `tools/bench/`
Not part of the firmware build. `flexnode-swd.ps1` (flash/diag), `build_fw.sh` + `export_bins.sh` (WSL), `ledtest/` (the 948 B bare-metal first-light program: GPIO + SysTick only, NOP-timed at 16 MHz HSI, proven to fade to off). `out/` is gitignored.

## Flash budget (the constraint)
App image 437,808 B at `ef0d948`, ~16 KiB free before the config page. Cheapest reclaim: the encoder drivers FlexNode never uses (BiSS-C, iC-PZ, MA732, AKSIM2, MA600, CUI AMT21/22, Orbis, AS5048, sine/cosine, quadrature, hall) in the aux port, all off the control path. Measure with the map file before cutting. See [`open-questions.md`](open-questions.md).
