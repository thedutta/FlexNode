# Flash findings — where the space actually went

_Measured 2026-09-07 01:20–02:05 IST against the `bee0685` image (app 437,808 B, 16,848 B free)._

Full analysis with tables: [`../docs/flash-budget.md`](../docs/flash-budget.md). This file is the
short version plus the reasoning traps, which is what a future session actually needs.

## The one-line answer

**There is a ~47 KiB hole in the flash map that nothing occupies**, and taking it needs no code
change at all. Add the measured code levers and it's **≥42 kB without touching the drive path**,
against 16,848 B free. The flash budget is no longer the constraint on anything.

## Finding 1 — the unused gap (the best one, and nearly missed)

Vectors are 472 B at 0x08000000. The CAN bootloader starts at 0x0800C000. **Nothing lives in
between: 0x080001D8–0x0800BFFF, 48,680 B.** `fw/stm32g474.ld` literally labels 0x8004000 and
0x8008000 "currently unused" and nobody read it.

Fix: extend `FLASH_ISR` LENGTH to 0xC000 and append `*(.rodata*)` (23,102 B) to the **`.isr_vector`
output section**, after `KEEP(*(.isr_vector))`.

> Gotcha, 2026-09-07 02:05 IST: it must go **inside `.isr_vector`**, not into a new section.
> `moteus_tool._read_elf` flashes sections **by name** from a fixed list, so a new section name is
> silently skipped — the firmware would link fine, flash "successfully", and not run.
> `export_bins.sh` already does `-j .isr_vector` so `out.08000000.bin` just grows.
> Never overlap 0x0800C000, and don't relocate `.ccmram`'s LMA (breaks `-j .ccmram`).

## Finding 2 — serialisation boilerplate is 18 % of the image

**79,518 B across the image is `SerializableHandler<T>` template instantiation.** Every struct
registered with `PersistentConfig` or `TelemetryManager` costs 2–8 kB in schema emission, text
parsing and binary round-trip.

How it surfaced — work backwards from something absurd. `ws2812_led.o` was **13.8 kB** for a driver
that blinks one LED:

| | Bytes |
|---|---|
| Actual LED logic (`PollMillisecond` 1,084, `RebuildFaultSchedule` 1,100, ctor 668, `ImuDemoPixel` 532, `Transmit` 456) | ~3,900 |
| `SerializableHandler<Config>` + `<Status>` — schema, text parsing, binary round-trip | ~8,400 |

Once that ratio is visible, `moteus_controller.o` being the largest object (129.5 kB) stops being
mysterious: 36,176 B of it is `AuxStatus`/`AuxConfig` serialisation alone.

**But the droppable subset is much smaller than 79 kB** — most registrations are load-bearing. See
the corrections below.

## Corrections — things I got wrong in this session

> Gotcha, 2026-09-07 02:05 IST: **`board_debug.o` is NOT droppable.** I recorded earlier that
> dropping it "does not break configuration or `moteus_tool`". Wrong. `moteus_tool.py` issues
> `d stop / pwm / cal / cali / ind / pos / vdq / index / rezero / cfg-set-output / reset` — **and
> `d flash`, which is how it enters the CAN bootloader.** Dropping it breaks `--calibrate` and
> `--flash`. It is also the motor test surface. **Keep it.**

> Gotcha, 2026-09-07 02:05 IST: **`Drv8323::Status` (7,990 B) downgraded from "safe" to
> "review required".** It is fault-bit readback, but `drv8323.o`'s Config archives are the
> mechanism by which `drv8323_conf.*` (gate-drive current, dead time, OCP, CSA gain) reaches
> `WriteConfig`. That is the drive path. Don't touch it casually.

> Gotcha, 2026-09-07 01:20 IST: the earlier plan in [`open-questions.md`](open-questions.md) said
> the first lever was "reclaim the unused encoder drivers". **Measured, the ones with standalone
> symbols total 19,242 B, and the rest measure exactly zero or are fully inlined** — `--gc-sections`
> had already removed anything unreferenced. Cutting there is also the only lever that makes future
> merges from mjbots/moteus harder. It is now *third*, not first.

The generalisable lesson: **measure before optimising.** Two intuitions in a row ("unused drivers
are dead weight", "the debug console is disposable") were reasonable and both wrong — the first
about magnitude, the second about dependencies.

## The levers, ranked

| # | Lever | Bytes | Risk |
|---|---|---|---|
| 1 | Linker-script gap → move `.rodata*` | **~23 kB now, ~47 kB available** | none, no code change |
| 2 | `--specs=nano.specs` + **`-u _printf_float` (mandatory)** | 28 kB exposed, saving unmeasured | libc swap only |
| 3 | `flexnode_nothrow_stubs.cc` — define the five `std::__throw_*` as `[[noreturn]]` traps | 11,357 B ceiling | none; behaviour under `-fno-exceptions` is terminate anyway |
| 4 | iC-PZ 6,637 · UART `kSerial` 5,917 · BiSS-C 3,832 · quadrature 1,816 · MA732 1,040 | **19,242 B measured** | off drive path; BiSS-C has a 1,056 B CCM ISR |
| 5 | `Ws2812Led::Status` telemetry | 4,524 B | ours, trivial |
| — | FOC stack for motor-less nodes (`bldc_servo.o` 104.5 kB) | large | **motor path — not before hardware validation** |

## Do not chase

- `.ccmram` **29,960 B** is hot ISR code copied to CCM RAM at boot (mbed
  `linker_script.ld.in:135-143` places it `>CCMRAM AT> FLASH`; `moteus.cc:157` memcpys it).
  **Unreclaimable.**
- `drv8323.o` 33,160 B, `board_debug.o` 13,568 B, `motor_position.h`, `bldc_servo.o` — ⚠ all
  drive/position path.
- `moteus_controller.o` at `-Os` looks tempting (129.5 kB at `-O3`) but that TU **instantiates CCM
  ISRs** (`ISR_UpdateSources`, `ISR_MaybeFinishSample`, `ISR_I2C_Update`). A TU-wide `-Os` slows
  the position-sampling ISR. ⚠ Review required.
- **No LTO.** Not near ISRs and weak symbols on a board that has not turned a motor.

## Recipe to re-measure

Run from WSL (**use a script file — `wsl.exe bash -c '...$VAR...'` mangles variables**, see
[`build-and-flash.md`](build-and-flash.md)). From Git Bash, prefix `MSYS_NO_PATHCONV=1` or the
`/mnt/...` argument gets rewritten into a Windows path.

```bash
NM=bazel-moteus-r4-parent/external/com_arm_developer_gcc/bin/arm-none-eabi-nm
"$NM" -S --size-sort -td -C ../tools/bench/out/moteus.elf > /tmp/syms.txt

# total serialisation cost
grep SerializableHandler /tmp/syms.txt | awk '{s+=$2+0} END{print s}'

# cost per registered struct type
grep "SerializableHandler<" /tmp/syms.txt | awk '{
  sz=$2+0; i=index($0,"SerializableHandler<"); rest=substr($0,i+20);
  j=index(rest,">::"); if(j>0){ agg[substr(rest,1,j-1)]+=sz }
} END{ for(k in agg) printf "%8d  %s\n", agg[k], k }' | sort -rn | head -25
```

For per-object attribution, relink manually with the exact Bazel link line (`bazel aquery`) and
`-Wl,-Map`. That reproduced every section size to the byte and is worth the effort — symbol sums
alone can't tell you which object a symbol came from.

## Consequence for the CAN layer

Profiles ([`../docs/can-layer.md`](../docs/can-layer.md) §6) were designed as the release valve for
the flash budget. **They are no longer load-bearing for space** — the whole v2 channel model, load
cell, servo and ToF work fits in one image with room to spare. They stay in the design because a
motor-less node that never links the FOC stack is a safety property and a product decision, not a
byte-count workaround.
