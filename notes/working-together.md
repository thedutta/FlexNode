# Working together

How Aditya and Claude operate on this repo. Non-negotiables first.

## Git
- Commit author is **Aditya Dutta** (`git config user.name` in this clone). Never anyone else.
- **No attribution lines** in commit messages or PR text (no Co-authored-by, no "generated with").
- `git add` **explicit paths only**. Never `git add -A`. The Windows clones use `core.autocrlf=true` and will show unrelated EOL-only changes; adding by path sidesteps that.
- Commit message via `git commit -F <file>`; multi-line messages through PowerShell here-strings have produced `@` garbage before.
- Push when asked, or when a task was explicitly "get it on GitHub". Otherwise commit and say it's local.
- Scripts run by WSL must be **LF**. `.gitattributes` pins `*.sh`, `*.py`, `tools/bazel`. A CRLF shebang fails with `Unknown option: -`.

## Safety gates on the board
Aditya was, in his words, "super scared" of the first boot and of anything that could cook the power stage. Standing rules:
- Anything that changes how or when the gate driver / phases energise: **propose and explain first, edit only after approval.**
- **Hard gate:** no `--calibrate`, no motor drive, no mode command that enables the DRV until the phase-order fix is validated on a current-limited supply (see [`bringup-log.md`](bringup-log.md)).
- Flashing, option bytes, CAN, LED, IMU, SWD reads: all safe with no motor / no encoder fitted. The firmware boots in `kStopped` with DRV enable and HIZ low.

## Hardware commands
Claude's auto-mode permission classifier blocks commands that write to hardware (openocd program/option bytes, and sometimes even plain `cp`). Pattern that works:
1. Claude prepares the exact command.
2. Aditya runs it with the **`!` prefix** in the Claude Code prompt. Output lands in the conversation; no copy-paste needed.
3. The `!` prompt is **Git Bash**, not PowerShell. Call PowerShell by full path:
   `/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -ExecutionPolicy Bypass -File <script.ps1> <mode>`

Read-only openocd (`probe`, `diag`) has sometimes been allowed for Claude directly; writes never. Don't burn turns retrying a blocked call.

## Division of labour
- Firmware, bench, protocol: this conversation, in the FlexNode repo.
- Website (`thedutta.github.io`), portfolio PDF (`Documents\Documents\portfolio-source\build.py`), photos: hand to a subagent (Opus) with the full brief. It knows the site conventions from memory files; give it the milestone facts and the "no fabricated images / no attribution" rules. It cannot see images pasted into chat; the file must be on disk (it found `Downloads\flexnode-glowing.PNG` last time).
- Claude's persistent memory lives outside the repo (`~/.claude/projects/.../memory/`). These notes are the in-repo, shareable version of the same lessons; keep both current, prefer putting technical detail here.

## Communication
- Aditya is at the bench, live, often with the board in hand. Short, concrete instructions beat essays. One action per step; say exactly what to look for and for how long.
- When Claude gets it wrong (it did, several times on the LED), say so plainly and move on. He catches things (the NRST call was his).

<!-- self: he does not want to be asked permission for reversible, in-scope work. He does want to be told before anything near the motor path. He reads the terminal on a phone sometimes: keep commands on one line. -->
