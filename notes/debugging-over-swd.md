# Debugging over SWD

No CAN adapter yet, so SWD is the only window into the running firmware. These are the moves that worked on 2026-09-06.

## Toolkit (all in `flexnode-swd.ps1`)
- **`probe`**: link sanity. If it prints the chip ID, the wiring is good. First thing after touching the harness.
- **`diag`**: halts and prints PC ten times over ~2 s. Varied addresses across the app (0x0803…–0x0806…), some in `Handler PendSV`, some in CCM RAM (0x1000…, the control ISR) = healthy main loop. Ten identical addresses = stuck there; map the address with `arm-none-eabi-addr2line -e moteus.elf <addr>`.
- **Read live variables by symbol.** For anything that isn't a global (most of the moteus object graph is pool-allocated), add a temporary namespace-scope `volatile uint32_t g_xxx_dbg[N]` in the relevant `.cc`, fill it each poll, build, then:
  ```
  arm-none-eabi-nm moteus.elf | grep xxx_dbg      # → 20000d60 B _ZN6moteus9g_led_dbgE
  ```
  and add a one-off script mode that does `halt; mdw 0x20000d60 N; resume`, twice with a `sleep` between, so you see what moves. Strip the array before committing. This is what finally cracked the LED (see [`ws2812-led.md`](ws2812-led.md)).
- Toolchain binaries live in the Bazel external tree: `moteus-r4-parent/bazel-moteus-r4-parent/external/com_arm_developer_gcc/bin/arm-none-eabi-{nm,objdump,readelf,addr2line,gdb}`. Call them from a script file, not an inline `wsl.exe bash -c`.

## What openocd errors actually meant here
| Message | Meant |
|---|---|
| `init mode failed (unable to connect to the target)` | SWD signal path broken: a wire off, DIO/CLK swapped or shifted, marginal GND, or NRST pulled while the ST-Link was powered. Board keeps running; the LED stays lit. Fix wiring, then `probe`. |
| `Fail reading CTRL/STAT register. Force reconnect` mid-program, then `Programming Failed` | Link dropped during the write. Every time it was physical: power pulled, or the board handled. App region now partial. Reseat, `probe`, reflash. |
| `couldn't bind gdb to socket on port 3333` | Stale openocd process, or another program on 3333. The script now disables that port. |
| `target was in unknown state when halt was requested` | Normal on a running target. Ignore. |
| `Target voltage: 2.3` | ST-Link's floating VTref pin. Ignore. |
| `Unable to match requested speed 2000 kHz, using 1800 kHz` | Normal for the clone. Ignore. |

## Reasoning traps this session fell into
Recorded because each one cost real time.
1. **"Steady colour" ≠ "static image".** An LED holding a colour after a reflash looked like the reset not taking. It was the WS2812 never receiving a valid off frame. Before blaming the reset path, check whether the firmware *thinks* it has sent something different from what you see.
2. **Don't assume a dark LED means a stopped MCU.** The NRST-hold made the board dark while perfectly fine. Separate "is it powered/running" (diag, probe) from "is the LED being driven correctly".
3. **Correlation with the debugger is ambiguous.** "Works only with the ST-Link plugged in" can be DWT (debug-only counter), NRST (unpowered ST-Link holds reset), or 3V3 back-power. Test them one at a time; here it was NRST, and the DWT theory was wrong.
4. **Verify passed ≠ the behaviour you're watching is new.** It was, every time; the *display* was stale, not the image. But do check `ms` uptime in a symbol read to be sure a fresh boot happened.

<!-- self: reading `status_.frames` and `want[0]` side by side was the decisive pair: "wants 0, frames stopped, LED still lit" isolated the transmit/physical layer from the render logic in one read. Reach for that shape of diagnostic early next time. -->
