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
	BOOT_HDR=2
	BOOT_PAGESIZE=4096
	BOOT_BASE=0
	BOOT_KDELTA=0x8000
	BOOT_RDELTA=0x1000000
	BOOT_TAGS=0x100
	;;
ugglite)
	BOARD_DEFS="-DBOARD_UGGLITE"
	DTB="${DTB:-../dtb/ugglite.dtb}"
	# The stock MSM8917 boot.img is header v0, 2048-byte pages, kernel at
	# 0x80008000, and the DTB appended after the ramdisk (not a v2 --dtb
	# section). Match it: this ABL predates header v2.
	BOOT_HDR=0
	BOOT_PAGESIZE=2048
	BOOT_BASE=0x80000000
	BOOT_KDELTA=0x8000
	BOOT_RDELTA=0x1000000
	BOOT_TAGS=0x100
	;;
qemu)
	BOARD_DEFS="-DBOARD_QEMU"
	DTB="${DTB:-}"
	;;
*)
	echo "error: unknown board '$BOARD' (sweet|ugglite|qemu)" >&2
	exit 1
	;;
esac

# EXTRA_CFLAGS lets a harness override board constants, e.g.
#   EXTRA_CFLAGS=-DFB_PHYS=0x60000000 ./build.sh "" qemu
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -fno-jump-tables -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra $BOARD_DEFS ${EXTRA_CFLAGS:-}"

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

# QEMU loads the raw Image itself; every phone target needs a boot.img.
if [ "$BOARD" = qemu ]; then
	cp out/Image out/shim-qemu.img
	ls -l out/shim-qemu.img
	exit 0
fi

[ -f "$DTB" ] || { echo "error: DTB not found: $DTB (set DTB=...)" >&2; exit 1; }

if [ "$BOOT_HDR" = 2 ]; then
	mkbootimg --header_version 2 --kernel out/Image --ramdisk out/ramdisk.bin \
		--dtb "$DTB" --base "$BOOT_BASE" --kernel_offset "$BOOT_KDELTA" \
		--ramdisk_offset "$BOOT_RDELTA" --tags_offset "$BOOT_TAGS" \
		--dtb_offset 0x1f00000 --pagesize "$BOOT_PAGESIZE" \
		--os_version 17.0.0 --os_patch_level 2035-06 \
		--cmdline "console=null" -o "out/shim-boot-$BOARD.img"
else
	# Header v0 and this lk wants Image.gz-dtb: a GZIP-compressed kernel with
	# the DTB appended after the gzip stream. lk decompresses the kernel and
	# then looks for the DTB past it.
	#   - raw Image        -> lk cannot decompress, reports "dtb not found"
	#                         or hangs (both seen on ugglite 2026-10-20)
	#   - DTB after image  -> same, the DTB is not where lk looks
	# gzip -n: no timestamp/filename, so builds are reproducible.
	#
	# The payload cannot ride in the ramdisk: measurement on ugglite showed
	# lk does not place it at ramdisk_addr (0x81000000 reads back unrelated
	# data, and a 64 KiB test ramdisk was not found there either). lk DOES
	# decompress the kernel, and the decompressed image is placed
	# contiguously in RAM, so the payload is appended to the uncompressed
	# Image before gzip - the shim then finds it right after its own _end.
	if [ -n "$PAYLOAD" ]; then
		cp "$PAYLOAD" out/payload-appended.bin
		# Pad to a 16 KiB boundary so the shim's _end-relative search lands
		# on the magic.
		cur=$(stat -c%s out/Image)
		pad=$(( (16384 - (cur % 16384)) % 16384 ))
		head -c "$pad" /dev/zero >> out/Image
		cat out/payload-appended.bin >> out/Image
		# image_size must describe the whole appended image, not just the
		# shim: the header field is still _end from the link, and lk uses it
		# to know how far the kernel extends.
		python3 - "$(stat -c%s out/Image)" <<'PY'
import struct, sys
total = int(sys.argv[1])
with open('out/Image', 'r+b') as f:
    f.seek(8)
    f.write(struct.pack('<Q', total))
PY
	fi
	gzip -9 -n -c out/Image > out/kernel.gz
	cp out/kernel.gz out/kernel-dtb
	cat "$DTB" >> out/kernel-dtb
	mkbootimg --header_version 0 --kernel out/kernel-dtb --ramdisk out/ramdisk.bin \
		--base "$BOOT_BASE" --kernel_offset "$BOOT_KDELTA" \
		--ramdisk_offset "$BOOT_RDELTA" --tags_offset "$BOOT_TAGS" \
		--pagesize "$BOOT_PAGESIZE" \
		--cmdline "console=null" -o "out/shim-boot-$BOARD.img"
fi

# Host render of the console layout (sweet geometry only).
if [ "$BOARD" = sweet ] && command -v gcc >/dev/null; then
	gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
fi

ls -l out/Image "out/shim-boot-$BOARD.img"
