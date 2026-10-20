#!/bin/bash
# Build the shim and wrap it in a boot.img v2 that mirrors the stock header.
#
#   ./build.sh [PAYLOAD] [BOARD]
#
# PAYLOAD  optional xnu payload from tools/mkpayload.py; without it the shim
#          runs its own diagnostics (for ugglite: the board probe).
# BOARD    "sweet" (default) or "ugglite". Selects shim/board.h constants and
#          the DTB that goes into the boot image.
#
# Cross compiler: aarch64-linux-gnu-gcc if present, else clang with
# --target=aarch64-none-elf.
set -euo pipefail
cd "$(dirname "$0")"

PAYLOAD="${1:-}"
BOARD="${2:-sweet}"

case "$BOARD" in
sweet)
	BOARD_DEFS=""
	DTB="${DTB:-../dtb/boot_unpacked/dtb}"
	;;
ugglite)
	BOARD_DEFS="-DBOARD_UGGLITE"
	DTB="${DTB:-../dtb/ugglite.dtb}"
	;;
*)
	echo "error: unknown board '$BOARD' (sweet|ugglite)" >&2
	exit 1
	;;
esac

CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -fno-jump-tables -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra $BOARD_DEFS"

if command -v aarch64-linux-gnu-gcc >/dev/null; then
	CC=aarch64-linux-gnu-gcc
	OBJCOPY=aarch64-linux-gnu-objcopy
	READELF=aarch64-linux-gnu-readelf
	NM=aarch64-linux-gnu-nm
	# GNU ld warns about the RWX segments shim.ld produces; lld has no such flag.
	LDFLAGS="-Wl,--no-warn-rwx-segments"
else
	CC="clang --target=aarch64-none-elf"
	OBJCOPY=llvm-objcopy
	READELF=llvm-readelf
	NM=llvm-nm
	CFLAGS="$CFLAGS -fuse-ld=lld"
	LDFLAGS=""
fi

SRCS="start.S shim.c console.c mmu.c fdt.c loader.c ufs.c probe.c"
OBJS=""
mkdir -p out
for f in $SRCS; do
	o="out/${f%.*}.o"
	$CC $CFLAGS -c "$f" -o "$o"
	OBJS="$OBJS $o"
done

# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$($READELF -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o out/probe.o 2>/dev/null | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }

$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none $LDFLAGS -Wl,-N \
	$OBJS -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$($NM out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image

# Ramdisk: an xnu payload if given, else an empty newc cpio (ABL dislikes a
# zero-size ramdisk on some builds) -> diagnostics.
if [ -n "$PAYLOAD" ]; then
	cp "$PAYLOAD" out/ramdisk.bin
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi

[ -f "$DTB" ] || { echo "error: DTB not found: $DTB (set DTB=...)" >&2; exit 1; }

mkbootimg --header_version 2 --kernel out/Image --ramdisk out/ramdisk.bin \
	--dtb "$DTB" --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
	--tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
	--os_version 17.0.0 --os_patch_level 2035-06 \
	--cmdline "console=null" -o "out/shim-boot-$BOARD.img"

# Host render of the console layout (sweet geometry only).
if [ "$BOARD" = sweet ] && command -v gcc >/dev/null; then
	gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
fi

ls -l out/Image "out/shim-boot-$BOARD.img"
