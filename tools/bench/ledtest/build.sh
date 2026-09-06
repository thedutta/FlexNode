#!/bin/bash
# Build the 948 B bare-metal WS2812 first-light test (GPIO + SysTick only,
# 16 MHz HSI, NOP-timed). Uses the arm toolchain from the Bazel external
# tree, so run tools/bench/build_fw.sh at least once first.
#   wsl.exe bash -c 'bash /mnt/c/Users/adity/Documents/CATBOT/FlexNode/Dev/moteus/tools/bench/ledtest/build.sh'
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
TC="$REPO/moteus-r4-parent/bazel-moteus-r4-parent/external/com_arm_developer_gcc/bin"
cd "$HERE"
"$TC/arm-none-eabi-gcc" -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -O2 -g -ffreestanding -nostdlib -nostartfiles -fno-builtin -Wall -Wextra -T link.ld -Wl,--gc-sections -Wl,-Map=ledtest.map main.c -o ledtest.elf
"$TC/arm-none-eabi-objcopy" -Obinary ledtest.elf ledtest.bin
"$TC/arm-none-eabi-size" ledtest.elf
ls -la ledtest.bin
