# Firmware flash optimisation — what to do, and what not to delete

_Standing report for Aditya. Measured 2026-09-07 01:20–02:40 IST against the `bee0685` image
(app 437,808 B, 16,848 B free). Full numbers and per-object tables live in
[`docs/flash-budget.md`](../docs/flash-budget.md); this report is the decision document._

## The short version

You do not have to remove a single driver to solve the flash problem.

Three build-level changes, none of which touches the motor drive path and none of which deletes
any functionality, were measured by real builds of an untouched copy of the tree:

| Change | Measured saving | What it deletes |
|---|---|---|
| Link against newlib-nano (the small C library) | −33,916 B | nothing |
| Stop the C++ exception runtime from being linked | −12,316 B (less on top of nano) | nothing |
| Compile one non-realtime file for size instead of speed | −50,144 B | nothing |
| **All three together** | **−84,028 B → 353,780 B image, 100,876 B free** | **nothing** |

That is six times the space you have today, before touching a line of moteus's driver code.
There is a further ~47 kB of physically unused flash between the vector table and the bootloader
that a linker-script change can reclaim, also without deleting anything.

Driver removal is an optional, last-resort lever. Everything measurable in that category adds up
to **19,242 B** — less than a quarter of what the build-level changes give — and it is the only
lever that makes future merges from upstream moteus harder. My recommendation, agreeing with the
coordinator: **keep every driver, take the 84 kB, revisit only if you ever actually run out.**

## What the "removable" drivers actually are

You said: _"I barely understood what drivers are being removed and why … for all we know it does
most of what we're trying to build anyway."_ That instinct is partly right, so here is each
candidate in plain terms. "Could FlexNode use it" means: is there a plausible CATBOT configuration
where this code earns its place.

**iC-PZ (6,637 B).** A driver for the iC-Haus iC-PZ, a high-resolution *optical* absolute encoder
chip that reads a reflective code disc over SPI and has its own calibration register set. It is
used in precision industrial and research actuators. FlexNode's planned encoders are magnetic
(AS5047 onboard, AS5600-class or MT6701 on I²C); an optical disc assembly is not on the roadmap.
This is the one candidate with no plausible CATBOT use. Losing it loses only iC-PZ support. It is
also the cleanest cut technically (its own telemetry channel, no interrupt entanglement).

**BiSS-C (3,832 B plus an unmeasured share of shared code).** Not a chip but a serial *protocol*
used by industrial absolute encoders from Renishaw, RLS, Hengstler and many Chinese servo-encoder
vendors. moteus implements it by bit-banging with a timer and DMA. Nothing in the CATBOT parts
list speaks BiSS-C, and the encoders you are considering (AS5600, MT6701, AS5047) do not. Losing it
loses the ability to bolt an industrial BiSS-C encoder onto an aux port later. Plausible but
unlikely for this robot.

**MA732 (1,040 B).** A driver for the MPS MagAlpha MA732, a cheap, popular SPI magnetic encoder in
the same class as the AS5047. **This one is potentially useful.** If a future joint encoder on the
SPI-capable aux pins ends up being a MagAlpha part — a very normal choice — this 1 kB is exactly
the code you would want. Keep it.

**Quadrature, software and hardware-timer (1,816 B).** Reads incremental A/B (ABZ) pulse trains —
the output mode of classic incremental encoders and also an *alternate output mode* of both the
AS5047 and the MT6701. **Potentially useful**: it is the natural way to give a second axis (for
example the SimpleFOC Mini's motor) a position feed without another SPI bus. Keep it.

**UART "serial" mode — the fdcanusb-over-UART server (5,917 B).** Lets a host computer talk the
same ASCII protocol that the fdcanusb CAN adapter speaks, but over a plain UART on an aux pin. In
other words: `moteus_tool` and tview without a CAN adapter. **You currently have no CAN adapter on
the bench.** If any USART-capable aux pin is reachable on FlexNode (the stock table lists PB3/PB4/
PA15 for USART2 and PB8/PB9 for USART3, the latter being the I²C bus, which becomes free on a node
without the IMU), this driver is a bring-up tool, not dead weight. Keep it, and consider using it.

**The rest — hall, index, sine/cosine, PWM-input, step/dir, AS5048, MA600, AksIM-2, CUI AMT21/22,
Orbis.** These have no standalone code at all any more: the linker's dead-code removal and the
compiler's inlining have already folded them into shared functions, so their individual cost is
*unmeasured and unmeasurable without per-driver experimental builds*. Several are plausibly useful
(hall sensors as a commutation fallback, PWM-input for the PWM mode of AS5600/MT6701, AS5048B as an
I²C encoder sibling of the AS5600, MA600 as MA732's sibling). Treat them as free.

Net: of the five measurable candidates, three (MA732, quadrature, UART serial) are things CATBOT
might genuinely want, one (BiSS-C) is unlikely but harmless, and one (iC-PZ) is dead weight worth
6.6 kB. Your worry was justified.

## Decision table

| Option | Space gained | Capability lost | Merge cost with upstream | Verdict |
|---|---|---|---|---|
| **Keep every driver; apply the three build-level changes** | 84,028 B measured (100,876 B free); +~23–47 kB more from the linker gap | none | none — 2 lines in `fw/BUILD`, one new file, one scoped pragma | **Default. Do this.** |
| Cut the obvious (iC-PZ only) | +6,637 B on top | iC-PZ optical encoders | small, contained edit in `aux_port.h` | Only if you are ever within 7 kB of the wall |
| Cut everything measurable (iC-PZ, BiSS-C, MA732, quadrature, UART serial) | +19,242 B on top | MagAlpha SPI encoders, ABZ incremental input, serial bring-up without CAN, BiSS-C | medium and permanent: exhaustive `switch`es in upstream files, every future merge conflicts | Not recommended |
| Cut the inlined drivers too | unmeasured | hall fallback, PWM-mode encoders, AS5048/MA600 | high; touches the position-sampling interrupt path | Do not |

I agree with the coordinator's read. The only refinement: iC-PZ is a legitimate future cut, but it
buys 6.6 kB against a 100 kB margin, so it is not worth the merge friction today.

## The three build-level changes, in enough detail to apply

**1. newlib-nano.** The firmware currently links the full-size newlib C library because the
`nanospecs` toolchain feature exists in `rules_mbed` but was never enabled for the `moteus`
target. Nano is the same library built for size: its `printf` family, `strtod`, `malloc` and
locale support are a fraction of the full versions. The change is two lines on the `moteus`
target in `fw/BUILD`: add `"nanospecs"` to `features`, and add `linkopts = ["-Wl,-u,_printf_float"]`.

> **Why `-u _printf_float` is mandatory.** Nano's `printf` leaves out floating-point formatting
> unless you explicitly ask for it. moteus prints *every* float — `conf get`, `tel get` in text
> mode, `moteus_tool --dump-config` and `--restore-config`, tview — through `snprintf("%g")`.
> Without the flag the build succeeds, the image is 4,880 B smaller, and every float comes out
> wrong. It is a silent correctness bug, not a build error. (`_scanf_float` is not needed; nothing
> uses `scanf`.)

Verified in the variant build: float printing is present and the C++ exception runtime largely
disappears as a side effect, because nano's C++ library does not drag it in.

**2. The throw-stub file.** Although everything compiles with exceptions disabled, three helper
functions from the standard library (`std::__throw_out_of_range_fmt`, `__throw_length_error`,
`__throw_logic_error`, referenced from `moteus.cc` and two mjlib objects) pull in the whole
exception, RTTI, `std::string` and even `std::random_device` machinery from the prebuilt library.
A new FlexNode-owned file `fw/flexnode_nothrow_stubs.cc` defines those functions (plus the
`optional`/`variant` variants) as `[[noreturn]]` traps. Under `-fno-exceptions` the existing
behaviour on that path is already "terminate", so nothing observable changes. Add the file to
`MOTEUS_SOURCES` in `fw/BUILD`; no upstream file is edited.

**3. Size-optimise one file: `fw/moteus_controller.cc`.** All firmware files are built with `-O3`
(speed) via `features = ["speedopt"]`. That is right for the FOC interrupt in `bldc_servo.cc`. It is
wrong for `moteus_controller.cc`, which is command handling, register dispatch and the code that
serialises config and telemetry structures. Building just that file for size measured −50,144 B —
the single largest saving — and revealed that most of the apparent "driver bloat" was the speed
optimiser inlining the same configuration code over and over.

> **The fragility, and the remedy.** The measurement used a file-wide `#pragma GCC optimize("Os")`.
> The big interrupt routines that live in this file as templates came out byte-identical — but only
> because identical copies also exist in `bldc_servo.o`, and the linker happened to pick those
> `-O3` copies first. A few small interrupt helpers that exist *only* in this file (including the
> AS5047 SPI transfer used during position sampling) did get size-optimised. Do not rely on link
> order. Apply the pragma with `#pragma GCC push_options` / `pop_options` around the register
> read/write and command-handling code only, or split the file, and have the position-sampling
> path reviewed before the first motor spin.

Applying a size flag to the whole firmware, incidentally, does not even compile
(`bldc_servo_control.h:1137`, a section-type conflict) and would be wrong anyway.

**4. The flash gap (optional, no code).** Vectors occupy 472 B at 0x08000000 and the bootloader
starts at 0x0800C000; the 48,680 B between are unused — the linker script even labels them so.
Extending the `FLASH_ISR` region and placing read-only data inside the `.isr_vector` output section
reclaims up to ~47 kB. The one rule: it must stay *inside* `.isr_vector`, because `moteus_tool`
flashes sections by name from a fixed list and would silently skip a new one. Details in
[`docs/flash-budget.md`](../docs/flash-budget.md).

## What we learned

- **Measure before optimising.** Two reasonable intuitions were wrong in one evening: "the unused
  encoder drivers are the big win" (they total 19 kB; the build flags total 84 kB) and "the debug
  console is disposable" (see below).
- **Dead-code removal had already done the driver pruning.** `--gc-sections` has been on all
  along. Anything genuinely unreferenced was never in the image; what remains is referenced by
  configuration switches, and most of *that* was `-O3` inlining rather than driver logic.
- **An unused-looking thing can be load-bearing.** `board_debug` looked like a 13.6 kB developer
  console. `moteus_tool` uses its `d flash` command to enter the CAN bootloader, and its `d cal`
  family to calibrate. Removing it would have broken both `--flash` and `--calibrate`.
- **Serialisation boilerplate is large and mostly non-negotiable.** Every config or telemetry
  struct costs 2–8 kB of generated schema, text and binary code. Reducing struct field counts
  (including in FlexNode's own LED and register code) is a cheaper lever than deleting drivers.
- **The flash budget was the constraint shaping the CAN-layer design.** It no longer is; per-role
  images stay only as a safety property, not as a space workaround.

## What is unmeasured, and what it would take

- The split of the 36,176 B of aux-port serialisation between never-used and needed devices.
  Needs one experimental build per removed device — a real edit each, because the switches are
  exhaustive under `-Werror`.
- The individual cost of the fully inlined drivers (hall, index, sine/cosine, PWM-input, step/dir,
  AS5048, MA600, AksIM-2, CUI, Orbis). Same method.
- The cost of a second commutation path for the SimpleFOC Mini: an analogue-based budget of
  ~10–13 kB, not a build.
- CPU headroom for that second loop. The firmware has no interrupt-timing instrumentation; the only
  runtime metric is `system_info.idle_rate`, readable over SWD today or via CAN later.
- **Nothing in this report has been validated on a running board.** Every figure is from builds.

## Appendix — symbol-level evidence (for future sessions, not for the decision)

- Image composition: `.text` 404,428 · `.ARM.extab` 276 · `.ARM.exidx` 448 · `.data` 2,696 ·
  `.ccmram` 29,960 (hot ISR code copied to CCM RAM at boot; unreclaimable).
- Largest objects: `moteus_controller.o` 129,541 · `bldc_servo.o` 104,475 · libc 42,153 ·
  `drv8323.o` 33,160 · libmbed 28,234 · `moteus.o` 19,383 · `ws2812_led.o` 13,832 ·
  `board_debug.o` 13,568.
- Driver symbols: `SerializableHandler<IcPz::Status>` family 6,637; `UartFdcanusbMicroServer`
  5,917; `BissC` ctor 1,312 + `ISR_ProcessRead` 1,056 (CCM) + rest 3,832; `Stm32Quadrature` 1,816;
  `MA732::SetRegister` 1,040.
- Under `-Os`: `AuxPort::HandleConfigUpdate` 13,636 → 4,788;
  `AuxStatus::Serialize<BinarySchemaArchive>` 4,764 → 604.
- Variant ELFs, map and scripts are in the gitignored `tools/bench/out/` (`_scratch_V*.elf`,
  `_moteus.map`, `_scratch_build*.sh`, `_relink_map.py`).
