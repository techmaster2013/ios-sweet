#!/bin/bash
# Build the Pi 5 stage-1 shim as a raw arm64 Image the Pi firmware loads
# via config.txt "kernel=ios-pi5-shim.img". Needs gcc-aarch64-linux-gnu.
set -euo pipefail
cd "$(dirname "$0")"
CC=${CROSS_COMPILE:-aarch64-linux-gnu-}gcc
OBJCOPY=${CROSS_COMPILE:-aarch64-linux-gnu-}objcopy
NM=${CROSS_COMPILE:-aarch64-linux-gnu-}nm
READELF=${CROSS_COMPILE:-aarch64-linux-gnu-}readelf
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align \
	-fno-stack-protector -fno-tree-loop-distribute-patterns -mcpu=cortex-a76 -Wall -Wextra"
OBJS="start shim-main console fdt loader"
mkdir -p out
$CC $CFLAGS -c start.S -o out/start.o
$CC $CFLAGS -c main.c -o out/shim-main.o
$CC $CFLAGS -c console.c -o out/console.o
$CC $CFLAGS -c fdt.c -o out/fdt.o
$CC $CFLAGS -c loader.c -o out/loader.o
# Loaded wherever the firmware likes, no relocation pass: no absolute
# addresses in C objects (start.o's image_size is the one allowed).
abs=$($READELF -r out/shim-main.o out/console.o out/fdt.o out/loader.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	$(for o in $OBJS; do printf 'out/%s.o ' $o; done) -lgcc -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/ios-pi5-shim.img
# Pad to image_size so .bss and stacks exist as zeroed bytes.
size=$($NM out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/ios-pi5-shim.img
ls -l out/ios-pi5-shim.img
