/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ABL ramdisk, copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"
#include "mmu.h"

#define BOOT_LINE_LENGTH	608
/*
 * Hardware page size for the boot_args placement. sweet is a 16K kernel;
 * ugglite's Cortex-A53 has no 16K granule, so its kernel pages are 4K and
 * boot_args/topOfKernelData must be aligned to that.
 */
#if defined(BOARD_UGGLITE)
#define BOOT_PAGE		0x1000UL
#else
#define BOOT_PAGE		0x4000UL
#endif

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end)) {
		/*
		 * ugglite's lk does not report the ramdisk in /chosen and does
		 * not place it at the header's ramdisk_addr (measured), so the
		 * payload is appended to the kernel image instead and found
		 * relative to the shim's own load address.
		 */
		extern uintptr_t sweet_payload_addr(void);
		uintptr_t pa = sweet_payload_addr();

		if (!pa)
			return "no payload after shim";
		rd_start = pa;
		rd_end = pa + 0x2000000;	/* 32 MiB ceiling; size comes from the header */
	}
	con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);

	const struct payload *p = (const struct payload *)rd_start;
	if (!magic_ok(p->magic))
		return "no SWEETXNU payload";
	if (p->version != 1 || (p->adt_off & 7) || (rd_start & 7))
		return "bad payload header";

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	con_hex2("PHYS", "base=", p->phys_base, "size=", p->mem_size);
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);

	/* The payload must not overlap where the kernel goes. */
	if (rd_end > p->phys_base && rd_start < p->phys_base + p->mem_size)
		return "ramdisk overlaps kernel memory";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + BOOT_PAGE - 1) & ~(BOOT_PAGE - 1);
	uintptr_t top = ba_pa + BOOT_PAGE;

	zero64(ba_pa, BOOT_PAGE);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB_PHYS;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = FB_BPP * 8;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	/*
	 * Prove the copy landed: read the first word back from the entry point
	 * and from phys_base. If these do not match the payload's bytes, the
	 * destination is not writable RAM (an XPU carve-out, or the wrong base),
	 * which is a very different failure from a bad entry address.
	 */
	con_hex("WROTE", *(volatile uint32_t *)entry_pa);
	con_hex("EXPECT", *(const uint32_t *)(rd_start +
	    (p->segs[2].file_off + (p->entry_va - p->segs[2].vmaddr))));
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	/*
	 * Enter XNU with the MMU ON and its RAM mapped Normal write-back.
	 * With the MMU off every fetch is Device-nGnRnE, and instruction fetch
	 * from Device memory is constrained-unpredictable; on this A53 the
	 * branch to the entry point then produced no output at all. XNU's own
	 * start.s reprograms the MMU from scratch, so the mapping only has to
	 * be sane for the first few instructions.
	 */
	{
		struct mmu_info mi;
		uintptr_t lo = p->phys_base & ~0x3ffUL;
		uintptr_t hi = (p->phys_base + p->mem_size + 0x3ffUL) & ~0x3ffUL;

		mmu_enable_range(&mi, lo, hi);
	}

	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}

/*
 * Where is the payload?
 *
 * On sweet, ABL reports linux,initrd-start/end in /chosen and loader_boot()
 * uses that. ugglite's lk does not: its stock DTB's chosen node has only
 * bootargs, and measurement showed the ramdisk is not placed at the boot image
 * header's ramdisk_addr either (0x81000000 read back as unrelated data, and a
 * 64 KiB test ramdisk was not found there either). lk does reliably load and
 * decompress the KERNEL, so shim/build.sh appends the payload to the kernel
 * image and the shim looks for it relative to its own load address.
 *
 * The image is linked at 0, so `&_end` is the offset of the byte just past
 * the shim within the loaded image; the payload follows at a 16K boundary.
 */
uintptr_t
sweet_payload_addr(void)
{
	extern char _end[];
	/* Position independent: _end's address is base + its link-time offset. */
	uintptr_t self_end = (uintptr_t)_end;
	uintptr_t cand = (self_end + 0x3fff) & ~(uintptr_t)0x3fff;

	/*
	 * Check the version as well as the magic: the string "SWEETXNU" also
	 * appears in this file (magic_ok's own comparison), so the magic alone
	 * is not enough to identify a payload.
	 */
	for (int i = 0; i <= 8; i++) {
		uintptr_t a = cand + (uintptr_t)i * 0x4000;
		const struct payload *p = (const struct payload *)a;

		if (magic_ok(p->magic) && p->version == 1 &&
		    p->nsegs > 0 && p->nsegs < 64)
			return a;
	}
	return 0;
}
