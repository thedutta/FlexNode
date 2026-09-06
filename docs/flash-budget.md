# FlexNode — Flash Budget

_Measured 2026-09-07 01:20–02:05 IST against the `bee0685` image (app 437,808 B, 16,848 B free to the config page at 0x0807F000). Companion to [can-layer.md §6](can-layer.md#6-profiles-build-time-roles) and [firmware.md](firmware.md)._

**Headline: the flash problem is solved, and the best lever costs no code at all.** There is a
~47 KiB hole in the flash map that nothing occupies. Add the reclaimable code levers and the
realistic total is **≥42 kB without going anywhere near the drive path**, against 16,848 B free
today.

## How this was measured

Two independent passes, which is why the confidence levels below differ:

- `arm-none-eabi-nm -S --size-sort -td -C` over `tools/bench/out/moteus.elf`, aggregated by
  demangled symbol name.
- A **linker map from a manual relink of the exact Bazel link line**, which reproduced every
  section size to the byte, giving per-object attribution.

Section totals: `.text` 404,428 + `.ARM.extab` 276 + `.ARM.exidx` 448 + `.data` 2,696 +
`.ccmram` 29,960 = 437,808 B, an exact match to `out.08010000.bin`.

Build flags actually in effect (from `bazel aquery`): `fw/` TUs at `-O3` (`features=["speedopt"]`),
mjlib/mbed dependencies at `-Os`; `-fno-rtti -fno-exceptions -ffunction-sections -fdata-sections
-Wl,--gc-sections -static` all already on. **No `nano.specs`** (the `nanospecs` feature exists in
rules_mbed but is not enabled), **no LTO**.

## The ranking

| # | Candidate | Measured | Risk | Merge pain | Verdict |
|---|---|---|---|---|---|
| 1 | **Unused flash gap 0x080001D8–0x0800BFFF** | **48,680 B** exists; ~23 kB usable immediately | none | low — `fw/stm32g474.ld` only | **do first** |
| 2 | newlib-nano + `-u _printf_float` | 28 kB exposed; **saving unmeasured** | none (no ISR use) | trivial — 2 lines | measure, then do |
| 3 | C++ throw-stub chain | **11,357 B ceiling**; saving unmeasured | none | none — new FlexNode-owned file | do third |
| 4 | iC-PZ driver | **6,637 B** | none | medium | cut |
| 5 | UART `kSerial` / fdcanusb server | **5,917 B** | none | medium | cut |
| 6 | BiSS-C | **3,832 B** | low — 1,056 B is a CCM ISR | medium | cut |
| 7 | Quadrature sw/hw | **1,816 B** | none | medium | cut |
| 8 | MA732 | **1,040 B** | none | medium | cut |
| 9 | Droppable telemetry registrations | ~13–20 kB, **less than first thought** | mixed — see below | none | selective only |
| 10 | Remaining inlined drivers | **unmeasured** — no standalone symbols | position path | high | only with per-driver builds |
| — | `moteus_controller.o` at `-Os` | unmeasured | **instantiates CCM ISRs** | low | ⚠ DO NOT TOUCH WITHOUT REVIEW |
| — | `.ccmram` hot code | 29,960 B | FOC/position ISRs | — | **unreclaimable** |
| — | `drv8323.o` | 33,160 B | gate-driver config path | — | ⚠ DO NOT TOUCH WITHOUT REVIEW |
| — | `board_debug.o` | 13,568 B | `moteus_tool` depends on it | — | **keep** |

### 1. There is a 47 KiB hole in the flash map — take this first

The vector table occupies 472 B at 0x08000000. The CAN bootloader starts at 0x0800C000. **Nothing
lives in between**: 0x080001D8–0x0800BFFF, **48,680 B**, of which pages 1–23 (47,104 B) are cleanly
usable. `fw/stm32g474.ld` even labels 0x8004000 and 0x8008000 "currently unused".

Extend `FLASH_ISR` LENGTH to 0xC000 and append input sections — `*(.rodata*)` is 23,102 B and moves
with no code change — to the **`.isr_vector` output section**, after `KEEP(*(.isr_vector))`.

Why this stays compatible, all verified rather than assumed:

- `moteus_tool._read_elf` flashes sections **by name** (`.text .ARM.extab .ARM.exidx .data .ccmram
  .isr_vector`) at their LMA, so content inside `.isr_vector` is picked up unchanged.
- `can_bootloader.cc::MaybeEraseSector` erases and writes any 2 KiB page handed to it; the bank
  math handles both banks and it has no self-protection.
- `export_bins.sh` already does `-j .isr_vector`, so `out.08000000.bin` simply grows.
- `flexnode-swd.ps1` needs no change.

Constraints: keep it **inside `.isr_vector`** — a new section name would be silently skipped by
stock `moteus_tool`; never overlap 0x0800C000 (the LENGTH guard enforces it); do not relocate
`.ccmram`'s LMA (it would break `-j .ccmram` in the 0x08010000 image). The 0x08040000 dual-bank
boundary is irrelevant — the map is linear and the app already straddles it.

The bootloader itself uses 8,368 B of its 16 KiB window, and its address is pinned
(`MultiplexBootloader = 0x800c001`). The config page offers nothing further.

### 2. newlib — 42,153 B, and what actually pulls it

| Symbol group | Bytes | Pulled in by |
|---|---|---|
| `svfprintf` | 8,648 | `snprintf` — mjlib formats **every float with `"%g"`** (`serializable_handler_detail.h:464`), plus persistent_config, board_debug, clock_manager |
| `vfiprintf` | 4,840 | libc `assert.o` → `fiprintf` |
| `strtod` + `gethex` + `hexnan` | 7,180 | `std::strtof` in `fw/strtof.h` (`conf set`, `d` args) |
| `dtoa` + `mprec` | 7,428 | float printing |
| malloc family | 4,012 | mbed alloc wrappers |

`--specs=nano.specs` with **`-u _printf_float` (mandatory)** — `conf get`, `tel get`, `moteus_tool
--dump-config/--restore-config` and tview all read `%g` output. `_scanf_float` is not needed. Risk
is confined to a libc swap; nothing here is in an ISR, and mjlib's `Pool` covers most allocation.
**Saving is unmeasured** — nano ships its own smaller-but-nonzero float printer, so the realised
number will be well under the 28 kB exposure.

### 3. The C++ throw chain — 11,357 B ceiling

`-fno-exceptions` and `-fno-rtti` are on for every object, yet the map shows the exception runtime
linked in anyway. One root cause: `std::__throw_out_of_range_fmt` (from `moteus.o` and mjlib
`micro_server.o`) and `__throw_length_error` / `__throw_logic_error` (mjlib `error_code.o`) pull
`libstdc++(functexcept.o)`, which drags in `eh_personality`, `eh_throw`, `eh_alloc`, `eh_arm`,
`eh_terminate`, `bad_alloc`, `bad_cast`, `stdexcept`, `cow-string-inst`, `random.o`,
`system_error`, `tinfo`, plus libgcc's `unwind-arm` (2,784) and `pr-support` (1,032).

A FlexNode-owned `flexnode_nothrow_stubs.cc` defining those five `std::__throw_*` as
`[[noreturn]]` traps severs the whole chain **with zero upstream edits**. Under `-fno-exceptions`
the behaviour today is `terminate` anyway, so this changes nothing observable.

### 4–8. Encoder drivers — 19,242 B, measured

| Driver | Bytes | Note |
|---|---|---|
| iC-PZ | 6,637 | cleanest cut: unconditional `telemetry_manager->Register(...)` plus a `std::optional` member, **no ISR entanglement** |
| UART `kSerial` / `UartFdcanusbMicroServer` | 5,917 | |
| BiSS-C | 3,832 | 1,056 B of it is a CCM ISR |
| Quadrature sw/hw | 1,816 | |
| MA732 | 1,040 | |

**Hall, index, sine/cosine, PWM-in, step/dir, AS5048, MA600, AksIM-2, CUI AMT21/22 and Orbis have
no standalone symbols** — they are fully inlined into four `AuxPort` functions and into seven
archive instantiations of `AuxStatus`/`AuxConfig`. Field-count evidence from `aux_common.h`: of 88
fields in the sub-structs, **51 belong to devices FlexNode will never have** (UartEncoder 11,
BissC 11, Hall 7, Quadrature 6, SineCosine 6, Index 5, PwmInput 5) against I2C 27 + SPI 10 kept.
The 36,176 B of `AuxStatus`/`AuxConfig` serialization scales with those fields — but **the split
was not measured**, and exhaustive `switch`es under `-Werror` make this a real edit, not an
`#ifdef`.

> ⚠ `AuxPort::ISR_MaybeFinishSample` (3,156 B) and `MotorPosition::ISR_UpdateSources` (2,842 B)
> live in `.ccmram` and feed the position loop. Removing a driver's `case` from either is on the
> **position path** — review required, even though it is not the current loop.

### 9. Serialisation boilerplate — real, but a smaller prize than it first appeared

Aggregating `SerializableHandler<T>` across the image gives **79,518 B, 18 % of the firmware** —
every struct registered with `PersistentConfig` or `TelemetryManager` costs 2–8 kB in schema
emission, text parsing and binary round-tripping. The tell was `ws2812_led.o` at 13,832 B for a
driver whose actual logic is ~3,900 B; the other ~8,400 B was serialisation for its `Config` and
`Status`.

The pattern is real and worth knowing. **The droppable subset is smaller and riskier than the raw
number suggests**, and two entries have been downgraded on review:

| Type | Bytes | Verdict |
|---|---|---|
| `IcPz::Status` | 4,988 | **drop** — hardware absent, no ISR entanglement (counted in #4, not double-counted here) |
| `Ws2812Led::Status` | 4,524 | **drop** — ours, telemetry only |
| `Info` / `GitInfo` / `SystemInfoData` | 12,546 | probably droppable, but `firmware_info.o` and `system_info.o` back registers `moteus_tool --info` reads — **verify before cutting** |
| `Drv8323::Status` | 7,990 | ⚠ **downgraded to review-required.** Fault-bit readback, but `drv8323.o`'s Config archives are how `drv8323_conf.*` reaches `WriteConfig` — the drive path |
| `BoardDebug::Impl::Data` | 2,054 | **keep** — see the correction below |
| `BldcServoMotor`, `MotorPosition::Config`, `Drv8323::Config`, `Ws2812Led::Config` | — | **keep** — all written through `conf set` |

> **Correction.** An earlier revision of this document stated that dropping `board_debug.o`
> "does not break configuration or `moteus_tool`". **That is wrong.** `moteus_tool.py` issues
> `d stop / pwm / cal / cali / ind / pos / vdq / index / rezero / cfg-set-output / reset` — **and
> `d flash`, which is how it enters the CAN bootloader.** Removing `board_debug` breaks
> `--calibrate` and `--flash`. It is now marked keep. (tview uses only `tel`/`conf` and is
> unaffected, and the sub-commands `moteus_tool` never sends are trimmable in principle — but this
> file is the motor test surface, so it stays until the motor works.)

## Recommended order

1. **Linker-script gap.** +23 kB immediately by moving `.rodata*`, up to ~47 kB available. No code
   change; verify with `flash` then `verify`.
2. **Measure, then apply** newlib-nano and the throw-stubs: two lines and one new file, no upstream
   edits.
3. **iC-PZ, `kSerial` UART, BiSS-C, quadrature, MA732** — 19,242 B measured, contained edits in
   `aux_port.h`, off the drive path.
4. `Ws2812Led::Status` and the verified-safe subset of info channels.
5. Only then the inlined drivers and `-Os` on the controller TU, each with a build measurement and
   an ISR review.

**Realistic total without touching the drive path: ≥42 kB** (23,102 + 19,242 from steps 1 and 3
alone), rising toward ~66 kB if the full gap is used, plus whatever nano and the throw-stubs
measure. Everything in the DO-NOT-TOUCH rows — `bldc_servo.o`, `.ccmram`, `drv8323.o`,
`board_debug.o`, `motor_position.h` — is excluded from that figure.

## Fixed overhead — do not chase

`.ccmram` (29,960 B) is hot code copied to CCM RAM at boot: mbed `linker_script.ld.in:135-143`
places it `>CCMRAM AT> FLASH`, and `moteus.cc:157` `memcpy`s it at startup. 38 hot symbols
(`GlobalPendSv` 4,540, `ISR_DoPositionCommon` 3,932, `ISR_DoControl` 3,624,
`ISR_MaybeFinishSample` 3,156, `ISR_UpdateSources` 2,842, `ISR_DoCurrent` 2,648, …). It occupies
flash permanently and **cannot be reclaimed**, except `BissC::ISR_ProcessRead` (1,056 B), which
falls with candidate #6.

`drv8323.o` (33,160 B) is **22,716 B serialisation** + 7,252 B driver logic (`WriteConfig` 2,756,
ctor 1,508, `PollMillisecond` 1,248, `StartEnable` 1,096) + helpers. Not dump tables — the Config
archives are the mechanism by which gate-drive current, dead time, OCP and CSA gain reach the
hardware. ⚠ Drive path.

## What this changes about the CAN layer

[can-layer.md §6](can-layer.md#6-profiles-build-time-roles) presented build-time profiles as the
release valve for the flash budget. **They are no longer load-bearing for space.** The whole v2
channel model, load cell, servo and ToF work fits in one image with room to spare. Profiles stay
in the design because a motor-less node that never links the FOC stack is a *safety property* and
a product decision — and `full` is now comfortably plausible as the retail default.

## Unverified

- newlib-nano and throw-stub savings are **ceilings, not measurements**. Scratch builds were
  queued but had not completed.
- `-Os` on `moteus_controller.o` is unmeasured, and that TU instantiates CCM ISRs.
- Per-driver attribution stops at the five drivers with standalone symbols; the rest are inlined.
- The `Info`/`GitInfo`/`SystemInfoData` registrations are assumed droppable but not confirmed
  against what `moteus_tool --info` actually reads.
- **No claim here has been validated against a running board.** All of it is static analysis of a
  linked image plus one manual relink.
