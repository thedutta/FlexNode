# FlexNode — Flash Budget

_Measured 2026-09-07 against the `bee0685` image (app 437,808 B, 16,848 B free to the config page at 0x0807F000). Companion to [can-layer.md §6](can-layer.md#6-profiles-build-time-roles) and [firmware.md](firmware.md)._

**Headline: the flash problem is comprehensively solved.** Three changes that never touch the drive
path measure **−84,028 B** together, taking the image to 353,780 B and leaving **100,876 B free**.
Add the unused flash gap and the removable drivers and the ceiling is ~150 kB, against 16,848 B
free today.

## How this was measured

Everything below is from **real builds**, not estimates: an untouched `/tmp` copy of the tree whose
baseline reproduced 437,808 B exactly, then one variant build per candidate. Attribution comes from
a linker map produced by re-running the exact Bazel link line (reproduces every section to the
byte) plus `nm -S`.

Section totals: `.text` 404,428 + `.ARM.extab` 276 + `.ARM.exidx` 448 + `.data` 2,696 +
`.ccmram` 29,960 = 437,808 B.

Flags in effect (`bazel aquery`): `fw/` TUs `-O3` (`features=["speedopt"]`), dependencies `-Os`;
`-fno-rtti -fno-exceptions -ffunction-sections -fdata-sections --gc-sections -static` all on.
**No `nano.specs`, no LTO.**

## The ranking

| # | Candidate | Measured | Risk | Merge pain | Verdict |
|---|---|---|---|---|---|
| 1 | **newlib-nano** + `-Wl,-u,_printf_float` | **−33,916 B** | none | 2 lines in `fw/BUILD` | **do first** |
| 2 | **Flash gap 0x080001D8–0x0800BFFF** | gap **48,680 B**; `.rodata*` 23,102 B movable | none | ~5 lines in `fw/stm32g474.ld` | do second |
| 3 | **`std::__throw_*` stub TU** | **−12,316 B** alone | none | new file only | do third |
| 4 | `moteus_controller.cc` at `-Os` | **−50,144 B** | **yes — see below** | pragma or TU split | ⚠ real, but review first |
| **1+3+4** | **combined** | **−84,028 B → 353,780 B, 100,876 free** | as #4 | | |
| 5 | iC-PZ | −6,637 B | none | medium | cut |
| 6 | UART `kSerial` / fdcanusb server | −5,917 B | none | medium | cut |
| 7 | BiSS-C | −3,832 B | 1,056 B is a CCM ISR | medium | cut |
| 8 | Quadrature sw/hw | −1,816 B | none | medium | cut |
| 9 | MA732 | −1,040 B | none | medium | cut |
| 10 | Hall, index, sine/cos, PWM-in, step/dir, AS5048, MA600, AksIM-2, CUI, Orbis | **unmeasured** — inlined | position path | high | only with per-driver builds |
| — | fw-wide `-Os` | **does not compile** — `bldc_servo_control.h:1137` section type conflict | — | — | moot |
| — | `.ccmram` | 29,960 B | hot ISRs | — | **unreclaimable** |
| — | `drv8323.o` 33,160 · `board_debug.o` 13,568 | | drive path / `moteus_tool` needs it | — | ⚠ keep |

Per-object: `moteus_controller.o` 129,541 · `bldc_servo.o` 104,475 · libc 42,153 · `drv8323.o`
33,160 · libmbed 28,234 · `moteus.o` 19,383 · `ws2812_led.o` 13,832 · `board_debug.o` 13,568 ·
libgcc 8,452 · libstdc++ 6,409 · `firmware_info.o` 6,400 · `system_info.o` 4,820 · `uuid.o` 3,040.

### 1. newlib-nano — −33,916 B, measured

What libc's 42,153 B actually is: `svfprintf` 8,648 (via `snprintf` — mjlib prints **every float
with `"%g"`**, `serializable_handler_detail.h:464`), `vfiprintf` 4,840 (libc `assert.o`),
`strtod`+`gethex`+`hexnan` 7,180 (`std::strtof` in `fw/strtof.h`, used by `conf set`),
`dtoa`+`mprec` 7,428, malloc family 4,012.

Under nano, verified in the variant ELF: `_svfprintf_r` 496, `_printf_float` 1,104, `_dtoa_r`
2,988, `_strtod_l` 3,016, `_malloc_r` 248.

> **`-Wl,-u,_printf_float` is mandatory.** `conf get`, `tel get` text mode, `moteus_tool
> --dump-config/--restore-config` and tview all consume `%g` output. Without it the image is
> 4,880 B smaller **and floats print wrong** — a silent correctness bug, not a build failure.
> `_scanf_float` is not needed.

Side benefit: nano's libstdc++ drops most of the exception chain by itself
(`.ARM.exidx` 448→8, `.data` 2,696→968), which is why #3 measures smaller when applied on top.

### 2. The unused flash gap — 48,680 B

Vectors occupy 472 B at 0x08000000; the CAN bootloader starts at 0x0800C000; **nothing lives in
between**. `fw/stm32g474.ld` even labels 0x8004000 and 0x8008000 "currently unused".

Extend `FLASH_ISR` LENGTH to 0xC000 and append input sections — `*(.rodata*)` is 23,102 B — to the
**`.isr_vector` output section**, after `KEEP(*(.isr_vector))`.

Compatibility verified, not assumed: `moteus_tool._read_elf` flashes sections **by name** at their
LMA; `can_bootloader.cc::MaybeEraseSector` writes any page handed to it with no self-protection;
`export_bins.sh` already does `-j .isr_vector` so the image simply grows; `flexnode-swd.ps1` needs
no change.

Constraints: keep it **inside `.isr_vector`** — a new section name is silently skipped by stock
`moteus_tool` (links fine, flashes "fine", does not run); never overlap 0x0800C000; do not relocate
`.ccmram`'s LMA. The 0x08040000 bank boundary is irrelevant.

### 3. Severing the exception chain — −12,316 B

`-fno-exceptions` and `-fno-rtti` are on everywhere, yet the EH runtime links in anyway.
`std::__throw_out_of_range_fmt` (from `moteus.o`, mjlib `micro_server.o`) and
`__throw_length_error`/`__throw_logic_error` (mjlib `error_code.o`) pull `libstdc++(functexcept.o)`
→ `eh_personality`, `eh_throw`, `eh_alloc`, `eh_arm`, `eh_terminate`, `bad_alloc`, `stdexcept`,
`cow-string-inst`, `random.o`, `system_error`, `tinfo`, plus libgcc `unwind-arm` and `pr-support`.

A FlexNode-owned `flexnode_nothrow_stubs.cc` defining those five as `[[noreturn]]` traps severs it
with **zero upstream edits**; behaviour under `-fno-exceptions` is `terminate` anyway.

### 4. `moteus_controller.cc` at `-Os` — −50,144 B, with a real caveat

The single largest measured saving, and the most interesting result: **most of the apparent "driver
bloat" is `-O3` inlining, not driver code.** Under `-Os`, `AuxPort::HandleConfigUpdate` drops
13,636 → 4,788 and `AuxStatus::Serialize<BinarySchemaArchive>` drops 4,764 → 604. That is why this
out-earns removing drivers entirely.

> ⚠ **Review required, and the reason is subtle.** The big CCM ISR bodies came out byte-identical —
> but only because they are COMDAT (`W`) symbols also emitted by `bldc_servo.o`, whose `-O3` copies
> win by **link order**. That is a fragile guarantee. Small aux ISR helpers that exist *only* in
> this TU did change, including `Stm32Spi::start_write`/`finish_write` — the AS5047 SPI transfer in
> the position ISR.
>
> A safe version scopes the pragma (`push_options`) to the register/command and config/schema code,
> or splits the TU. **Do not apply a TU-wide `-Os` and assume the link order holds.**

### 5–9. Removable drivers — 19,242 B measured

iC-PZ 6,637 · UART `kSerial` 5,917 · BiSS-C 3,832 · quadrature 1,816 · MA732 1,040.

iC-PZ is the clean cut: an unconditional `telemetry_manager->Register(icpz_name, &ic_pz_->status_)`
plus a `std::optional` member, no ISR entanglement.

Hall, index, sine/cosine, PWM-in, step/dir, AS5048, MA600, AksIM-2, CUI and Orbis have **no
standalone symbols** — fully inlined into four `AuxPort` functions and seven archive instantiations
of `AuxStatus`/`AuxConfig`. Field evidence (`aux_common.h`): 51 of 88 sub-struct fields belong to
devices FlexNode will never have, against I2C 27 + SPI 10 kept. The split of that 36,176 B is
**unmeasured**.

> ⚠ `AuxPort::ISR_MaybeFinishSample` (3,156 B) and `MotorPosition::ISR_UpdateSources` (2,842 B) are
> in `.ccmram` and feed the position loop. Removing a driver's `case` from either needs review.

## Do not touch

- **`.ccmram` (29,960 B)** — hot ISR code copied to CCM RAM at boot (`linker_script.ld.in:135-143`
  places it `>CCMRAM AT> FLASH`; `moteus.cc:157` memcpys it). 38 symbols. **Unreclaimable**, except
  `BissC::ISR_ProcessRead` (1,056 B) which falls with #7.
- **`drv8323.o` (33,160 B)** = 22,716 B serialisation + 7,252 B logic. Not dump tables — the Config
  archives are how `drv8323_conf.*` (gate current, dead time, OCP, CSA gain) reaches the gate
  driver. ⚠ Drive path.
- **`board_debug.o` (13,568 B)** — **keep.** `moteus_tool.py` sends `d stop/pwm/cal/cali/ind/pos/
  vdq/index/rezero/cfg-set-output/reset` **and `d flash`, which is how it enters the CAN
  bootloader**. Removing it breaks `--calibrate` and `--flash`. It also energises the motor.
  (An earlier revision of this document wrongly called it disposable.)

## Recommended order

1. **newlib-nano + `-u _printf_float`** — −33,916 B for two lines.
2. **Linker gap** — +23 kB via `.rodata*`, up to ~47 kB available, no code change.
3. **Throw stubs** — one new file, no upstream edits.
4. **Controller-TU `-Os` with a scoped pragma**, after ISR review.
5. **iC-PZ, `kSerial`, BiSS-C, quadrature, MA732** — 19,242 B.

Steps 1+3+4 alone measure **84,028 B** (353,780 B image, 100,876 B free). With the gap, ~130 kB;
with step 5, ~150 kB. `bldc_servo.o`, `.ccmram`, `drv8323.o`, `board_debug.o` and
`motor_position.h` are excluded from every figure.

## Cost of the second commutation path (SimpleFOC Mini)

Now answerable: **it fits comfortably** once steps 1–3 land. Cost is **unmeasured**, but bounded by
analogues — `ISR_DoVoltageDQ` (the existing dq→PWM step) is 556 B, the I²C AS5600 path already
exists, and a registered Config+Status pair costs ~8.2 kB of archive instantiations (measured on
the LED's). Budget **~10–13 kB**.

**Write it separate; share nothing with `bldc_servo`.** Header-only `foc.h`/`math.h`
(`InverseDqTransform`, `Cordic`, `RadiansToQ31`) are reusable without touching the motor path. Two
hard constraints:

> ⚠ **The CORDIC is a single shared peripheral**, used inside the FOC ISR with a write→read
> sequence. A main-loop user gets pre-empted mid-transaction and reads someone else's result. The
> Mini path must use **software sin/cos or a LUT — never the shared CORDIC.** (`sinf`/`cosf` are
> not linked today; cost unmeasured.)

> `ConfigurePwmTimer` is private to `BldcServo::Impl`. Write a small separate TIM1 init.

**CPU headroom is unmeasured** — the code has no ISR cycle instrumentation. `pwm_rate_hz` defaults
to 30,000 for family 0 / rev 8, i.e. 5,667 cycles per period at 170 MHz for the ~30 kB CCM ISR, and
I²C polling already runs inside that ISR (`ISR_I2C_Update`). A Mini loop driven from
`PollMillisecond` is main-loop time, not ISR time. The one available runtime metric is
`system_info.idle_rate` (main-loop idle iterations/s, `system_info.cc:75`) — readable over SWD now
as a delta of `moteus::SystemInfo::idle_count`, or via `tel get system_info` once CAN is up.

## What this changes about the CAN layer

Profiles ([can-layer.md §6](can-layer.md#6-profiles-build-time-roles)) were the release valve for
the flash budget. **They are emphatically no longer load-bearing for space** — with ~100 kB free
after three drive-path-free changes, the whole v2 channel model, load cell, servo, ToF *and* a
second commutation path fit in one image. Profiles stay because a motor-less node that never links
the FOC stack is a safety property and a product decision. `full` is the retail default.

## Unverified

- The 36,176 B of `AuxStatus`/`AuxConfig` serialisation has not been split per driver.
- Candidates 10 (inlined drivers) remain unmeasured; they have no standalone symbols.
- The second-commutation-path figure is an analogue-based budget, not a build.
- CPU headroom for a second commutation loop is unmeasured.
- **No claim here has been validated against a running board.**
