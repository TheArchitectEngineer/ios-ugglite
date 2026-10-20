/*
 * EL1 stage-1 MMU bring-up for the shim, using the granule the board's CPU
 * actually implements.
 *
 * sweet (SM7150) has the 16K granule, which is what XNU is built for there:
 * T0SZ=28 gives a 36-bit VA whose walk starts at level 2, and one 16K table
 * of 2048 x 32MB blocks covers 64GB.
 *
 * ugglite (MSM8917, Cortex-A53) has only 4K and 64K, so the 4K layout is
 * used instead: T0SZ=28 starts the walk at level 1 with 1GB blocks, and one
 * 4K table of 512 entries covers 512GB.
 *
 * Qualcomm XPUs fault on speculative reads of protected carve-outs (TZ,
 * hyp, modem...), so nothing is mapped cacheable except our own image:
 *   - default: Device-nGnRnE, execute-never (same as MMU off)
 *   - the block(s) holding the image get an L3 table: image pages are
 *     Normal write-back, the rest of the block stays Device
 *   - the splash framebuffer block is Normal non-cacheable (write-combining)
 */
#include <stdint.h>
#include "mmu.h"

#if defined(BOARD_UGGLITE)
#define PG_SHIFT	12
#define NENT		512		/* 4K table = 512 x 8 bytes */
#define BLK_SHIFT	30		/* 1GB level-1 block */
#define TCR_TG0		(0UL << 14)	/* 4K */
#define MMU_COVER	(512UL << 30)	/* one L1 table covers 512GB */
#else
#define PG_SHIFT	14
#define NENT		2048		/* 16K table = 2048 x 8 bytes */
#define BLK_SHIFT	25		/* 32MB level-2 block */
#define TCR_TG0		(2UL << 14)	/* 16K */
#define MMU_COVER	(64UL << 30)	/* one L2 table covers 64GB */
#endif

#define PG_SIZE		(1UL << PG_SHIFT)
#define BLK_SIZE	(1UL << BLK_SHIFT)

#define ATTR_DEV	0	/* MAIR attr0 = 0x00 Device-nGnRnE */
#define ATTR_WB		1	/* MAIR attr1 = 0xff Normal WB RW-alloc */
#define ATTR_NC		2	/* MAIR attr2 = 0x44 Normal non-cacheable */
#define MAIR_VAL	0x44ff00UL

#define D_BLOCK		1UL
#define D_TABLE		3UL
#define D_PAGE		3UL
#define D_ATTR(i)	((uint64_t)(i) << 2)
#define D_SH_OUTER	(2UL << 8)
#define D_SH_INNER	(3UL << 8)
#define D_AF		(1UL << 10)
#define D_PXN		(1UL << 53)
#define D_UXN		(1UL << 54)

#define DEV_FLAGS	(D_ATTR(ATTR_DEV) | D_AF | D_PXN | D_UXN)
#define WB_FLAGS	(D_ATTR(ATTR_WB) | D_SH_INNER | D_AF | D_UXN)
#define NC_FLAGS	(D_ATTR(ATTR_NC) | D_SH_OUTER | D_AF | D_PXN | D_UXN)

/* TCR: T0SZ=28, IRGN0/ORGN0=WBWA, SH0=inner, TG0=board, EPD1, IPS=40 bit */
#define TCR_VAL		(28UL | (1UL << 8) | (1UL << 10) | (3UL << 12) | TCR_TG0 | \
				 (1UL << 23) | (2UL << 30) | (2UL << 32))

#define SCTLR_M		(1UL << 0)
#define SCTLR_A		(1UL << 1)
#define SCTLR_C		(1UL << 2)
#define SCTLR_I		(1UL << 12)
#define SCTLR_WXN	(1UL << 19)

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))

extern char _head[], _end[];

/* 1 top-level table + up to 2 L3 tables, plus slack to align at runtime. */
static uint64_t pt_pool[4 * NENT] __attribute__((aligned(4096)));

static void dcache_inval_range(uintptr_t start, uintptr_t end)
{
	uint64_t line = 4UL << ((rsr("ctr_el0") >> 16) & 0xf);

	for (uintptr_t a = start & ~(line - 1); a < end; a += line)
		__asm__ volatile("dc ivac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static void zero_table(uint64_t *t)
{
	for (int i = 0; i < NENT; i++)
		t[i] = 0;
}

int mmu_enable(struct mmu_info *info)
{
	uintptr_t img_lo = (uintptr_t)_head & ~(PG_SIZE - 1);
	uintptr_t img_hi = ((uintptr_t)_end + PG_SIZE - 1) & ~(PG_SIZE - 1);
	uintptr_t pool = ((uintptr_t)pt_pool + PG_SIZE - 1) & ~(PG_SIZE - 1);
	uint64_t *top = (uint64_t *)pool;
	int n_l3 = 0;

	info->img_lo = img_lo;
	info->img_hi = img_hi;
	info->ttbr = pool;
	if (((img_hi - 1) >> BLK_SHIFT) - (img_lo >> BLK_SHIFT) > 1)
		return -1;	/* only room for two L3 tables */

	/* Top level: Device everywhere RAM or MMIO can be, rest unmapped. */
	zero_table(top);
	for (uint64_t i = 0; i < MMU_COVER >> BLK_SHIFT; i++)
		top[i] = (i << BLK_SHIFT) | DEV_FLAGS | D_BLOCK;
	top[FB_PHYS >> BLK_SHIFT] = (FB_PHYS & ~(BLK_SIZE - 1)) | NC_FLAGS | D_BLOCK;

	/* L3 for each block that holds part of the image. */
	for (uint64_t blk = img_lo >> BLK_SHIFT; blk <= (img_hi - 1) >> BLK_SHIFT; blk++) {
		uint64_t *l3 = (uint64_t *)(pool + (1 + n_l3++) * PG_SIZE);
		uint64_t base = blk << BLK_SHIFT;
		for (int i = 0; i < NENT; i++) {
			uint64_t pa = base + ((uint64_t)i << PG_SHIFT);
			int ours = pa >= img_lo && pa < img_hi;
			l3[i] = pa | (ours ? WB_FLAGS : DEV_FLAGS) | D_PAGE;
		}
		top[blk] = (uintptr_t)l3 | D_TABLE;
	}
	info->n_l3 = n_l3;

	/* Image lines may be stale in the cache from ABL; we wrote memory directly. */
	dcache_inval_range(img_lo, img_hi);
	__asm__ volatile("ic iallu; dsb sy; isb" ::: "memory");

	wsr("mair_el1", MAIR_VAL);
	wsr("tcr_el1", TCR_VAL);
	wsr("ttbr0_el1", pool);
	__asm__ volatile("isb; tlbi vmalle1; dsb nsh; isb" ::: "memory");

	uint64_t sctlr = rsr("sctlr_el1");
	info->sctlr_before = sctlr;
	sctlr &= ~(SCTLR_A | SCTLR_WXN);
	sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
	wsr("sctlr_el1", sctlr);
	__asm__ volatile("isb" ::: "memory");
	info->sctlr_after = rsr("sctlr_el1");
	return 0;
}

/* Translate a VA with AT S1E1R; returns PAR_EL1. */
uint64_t mmu_probe(uintptr_t va)
{
	__asm__ volatile("at s1e1r, %0; isb" : : "r"(va) : "memory");
	return rsr("par_el1");
}
