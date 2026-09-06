# FlexNode build notes

Working notes for the FlexNode firmware and bench, kept so that each new session (human or Claude) starts with what the last one learned instead of re-deriving it. These are practitioner notes, not user docs; the polished story lives in [`../docs/`](../docs/).

**Read first, every session:** [`working-together.md`](working-together.md) (how we operate) and [`bringup-log.md`](bringup-log.md) (what is validated vs still gated).

| File | What it holds |
|---|---|
| [`working-together.md`](working-together.md) | Ground rules: commit identity, safety gates on motor work, the `!` prefix for hardware commands, subagent use |
| [`bench-setup.md`](bench-setup.md) | Physical bench: SWD header, the NRST rule, power, ST-Link driver, what "standalone" means |
| [`build-and-flash.md`](build-and-flash.md) | WSL build, image layout, the flash script and its modes, option bytes, flash budget |
| [`debugging-over-swd.md`](debugging-over-swd.md) | Diagnostic toolkit: PC sampling, reading RAM by symbol, what each openocd error actually means |
| [`ws2812-led.md`](ws2812-led.md) | The status LED: behaviour, why the timing is DWT-only, the "colour that never turns off" trap |
| [`firmware-map.md`](firmware-map.md) | Where things live in the fork, how to add a register / an aux I²C device / a config group |
| [`bringup-log.md`](bringup-log.md) | Chronological record of what was validated when, and the remaining hard gates |
| [`open-questions.md`](open-questions.md) | Decisions still pending: flash budget strategy, SimpleFOC topology, CAN adapter, encoder |

Conventions:
- Dates are absolute (YYYY-MM-DD). "Today" rots.
- A `> Gotcha:` callout marks something that already cost real time.
- `<!-- self: ... -->` HTML comments are notes-to-self for Claude; invisible on GitHub, visible in the source.
- When a note stops being true, edit it. Don't append contradictions.

Tooling referenced throughout lives in [`../tools/bench/`](../tools/bench/).
