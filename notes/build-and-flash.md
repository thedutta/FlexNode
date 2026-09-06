# Build and flash

## Build (WSL Ubuntu, from Windows)
The Bazel workspace is `moteus-r4-parent/`. Repo-pinned Bazel 7.4.1 via `tools/bazel`.

```
wsl.exe bash -c 'bash /mnt/c/Users/adity/Documents/CATBOT/FlexNode/Dev/moteus/tools/bench/build_fw.sh'
```

`build_fw.sh` runs `tools/bazel build --config=target //:target`, checks the real exit code (`${PIPESTATUS[0]}`, since `tail` lies), then calls `export_bins.sh` which objcopies the three flash images into `tools/bench/out/` (gitignored), mirroring upstream `fw/flash.py` exactly. A cached rebuild is ~25 s; a cold one a few minutes.

> Gotcha: `wsl.exe bash -c '...$VAR...'` from Git Bash mangles `$VAR` and `${PIPESTATUS[0]}` even inside single quotes. Anything non-trivial goes in a script file and is invoked as `bash /mnt/c/.../script.sh`. Also `wsl.exe bash -lc` (login shell) breaks here; use `bash -c`.

> Gotcha: Bash heredocs in the Claude tool occasionally fail with `unexpected EOF while looking for matching '` on larger bodies. Write files with the Write tool or a Python script file instead.

The build has `-Werror`. The moteus style rules apply: no trailing whitespace, blank lines truly empty. `grep -n -E "[[:space:]]+$" file` before committing.

## Image layout (512 KB G474, dual-bank, DBANK=1)
| Image | From | Address | Size (2026-09-06) |
|---|---|---|---|
| `out.08000000.bin` | `moteus.elf` `.isr_vector` | 0x08000000 | 472 B |
| `out.0800c000.bin` | `can_bootloader.elf` | 0x0800C000 | 8,368 B |
| `out.08010000.bin` | `moteus.elf` app sections | 0x08010000 | **437,808 B** |

Persistent config page is at **0x0807F000**. App window = 444 KiB → **~16 KiB free** as of `ef0d948`. Track this number in every commit message that changes firmware; it is the binding constraint (see [`open-questions.md`](open-questions.md)).

## Flash script
`tools/bench/flexnode-swd.ps1`, run from Git Bash (`!` prompt) as:

```
/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -ExecutionPolicy Bypass -File C:/Users/adity/Documents/CATBOT/FlexNode/Dev/moteus/tools/bench/flexnode-swd.ps1 <mode>
```

| Mode | Writes? | Does |
|---|---|---|
| `probe` | no | chip ID, FLASH_OPTR, first flash words. **Run after any harness change.** |
| `probeslow` | no | same at 300 kHz SWD for a marginal link |
| `diag` | no | halts and samples PC ten times over ~2 s: alive vs stuck, and where |
| `verify` | no | byte-compares flash against the three images |
| `flash` | yes | programs + verifies all three images, then reset |
| `optionbytes` | yes | clears nSWBOOT0 → boot from flash regardless of PB8. **Required once per chip**, or it lands in the ROM bootloader (PB8/BOOT0 is held high by the I²C pull-up). Done on the first article; FLASH_OPTR reads `0xFBEFF8AA` |
| `ledtest` | yes | programs the 948 B bare-metal WS2812 chime at 0x08000000 (first-light test, no moteus) |
| `erase` | yes | mass-erase bank 0. Never implied by anything else |

The script disables openocd's gdb/tcp/telnet ports (a stale openocd once held port 3333 and every run failed with `couldn't bind gdb to socket`). It uses `reset_config none separate`: SWD only, NRST not driven by openocd.

Expect `** Verified OK **` three times. Anything else, see [`debugging-over-swd.md`](debugging-over-swd.md).

## Workflow that worked
1. Edit → `build_fw.sh` → confirm `BUILD_RC=0` and the new image size.
2. Aditya runs `flash` via `!`.
3. If the change is visible on the LED, watch the first 3 s after the reset. If it isn't, use `diag` or a symbol read.
4. Commit only after the board confirms. Commit message states the image size.

<!-- self: I once flashed a "temporary default on" bench build (led.imu_demo=1) to test with no CAN adapter, then reverted the default and reflashed before committing. That pattern is fine; just never commit the bench default. -->
