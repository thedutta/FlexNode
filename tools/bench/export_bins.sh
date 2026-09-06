#!/bin/bash
# Export the three flash images from the Bazel build into tools/bench/out/,
# split exactly as upstream moteus-r4-parent/fw/flash.py does.
# Run inside WSL after tools/bazel build --config=target //:target
# (build_fw.sh does both).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
cd "$REPO/moteus-r4-parent"
OBJ=bazel-moteus-r4-parent/external/com_arm_developer_gcc/bin/arm-none-eabi-objcopy
OUT="$HERE/out"
mkdir -p "$OUT"
E=bazel-out/stm32g4-opt/bin/fw/moteus.elf
B=bazel-out/stm32g4-opt/bin/fw/can_bootloader.elf
$OBJ -Obinary -j .isr_vector "$E" "$OUT/out.08000000.bin"
$OBJ -Obinary -j .text -j .ARM.extab -j .ARM.exidx -j .data -j .bss "$B" "$OUT/out.0800c000.bin"
$OBJ -Obinary -j .text -j .ARM.extab -j .ARM.exidx -j .data -j .ccmram -j .bss "$E" "$OUT/out.08010000.bin"
rm -f "$OUT/moteus.elf"; cp "$E" "$OUT/moteus.elf"
ls -la "$OUT"/*.bin
md5sum "$OUT"/*.bin
APP=$(stat -c %s "$OUT/out.08010000.bin")
echo "app image ${APP} B; free to config page (0x0807f000): $((0x6F000 - APP)) B"
