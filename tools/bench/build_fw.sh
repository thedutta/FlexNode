#!/bin/bash
# Build the FlexNode firmware in WSL and export the flash images.
# From Windows (Git Bash / the ! prompt):
#   wsl.exe bash -c 'bash /mnt/c/Users/adity/Documents/CATBOT/FlexNode/Dev/moteus/tools/bench/build_fw.sh'
# Checks bazel's real exit code (tail would lie) and only exports on success.
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
cd "$REPO/moteus-r4-parent"
tools/bazel build --config=target //:target 2>&1 | grep -v -E "^\s*$" | tail -40
rc=${PIPESTATUS[0]}
echo BUILD_RC=$rc
if [ "$rc" = "0" ]; then
  bash "$HERE/export_bins.sh"
fi
exit $rc
