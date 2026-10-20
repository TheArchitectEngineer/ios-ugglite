/*
 * Board probe: the questions that decide whether XNU can be ported to a new
 * SoC, answered on the panel in one boot.
 *
 *   1. Does this CPU implement the 16K translation granule? XNU's SWEET board
 *      config is a 16K-page kernel, so this is the go/no-go question.
 *   2. What does the live FDT say about memory, reserved regions, the
 *      interrupt controller, the CPU count and the timer PPIs?
 *   3. Which GIC version is this, and is the CPU interface where we expect?
 *
 * Ordering matters: everything cheap and safe is printed first, because the
 * granule probe is the one step that can hang the CPU (see granule_test).
 *
 * Runs with the MMU off, so all memory is Device: only naturally aligned
 * accesses are used (-mstrict-align).
 */
#include <stdint.h>
#include "console.h"
#include "board.h"
#include "fdt.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

/* ---- GICv2 register map (qcom,msm-qgic2) ---- */
#define GICD_CTLR	0x000
#define GICD_TYPER	0x004
#define GICD_IIDR	0x008
#define GICD_PIDR2	0xfe8

#define GICC_CTLR	0x000
#define GICC_PMR	0x004
#define GICC_IAR	0x00c
#define GICC_HPPIR	0x018

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void __attribute__((unused)) wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

/* ---- page tables for the granule test ---- */

static uint64_t pt16k[2048] __attribute__((aligned(16384)));
static uint64_t pt4k[512] __attribute__((aligned(4096)));

#define D_BLOCK		1UL
#define D_ATTR(i)	((uint64_t)(i) << 2)
#define D_SH_INNER	(3UL << 8)
#define D_AF		(1UL << 10)
#define D_PXN		(1UL << 53)
#define D_UXN		(1UL << 54)
#define BLK_FLAGS	(D_ATTR(0) | D_SH_INNER | D_AF | D_PXN | D_UXN)

/* Clean a range to the point of coherency so the page-table walk sees it. */
static void clean_dcache(uintptr_t start, uintptr_t end)
{
	uint64_t line = 4UL << ((rsr("ctr_el0") >> 16) & 0xf);

	for (uintptr_t a = start & ~(line - 1); a < end; a += line)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Identity-map with the given granule and translate `va` with AT S1E1R.
 * Returns PAR_EL1: bit 0 (F) set means the walk failed.
 *
 * T0SZ = 28 (36-bit VA) for both granules, so the walk starts at level 1
 * for 4K (1GB blocks) and level 2 for 16K (32MB blocks).
 *
 * CAUTION: if the CPU does not implement the granule named in TCR_EL1.TG0,
 * that register becomes UNKNOWN and the AT may hang or fault unpredictably.
 * Only call with sixteen=1 when the ID register says it is supported.
 */
static uint64_t granule_test(int sixteen, uintptr_t va)
{
	uint64_t tcr = 28UL;			/* T0SZ = 28 */
	uint64_t par;

	wsr("mair_el1", 0xffUL);		/* attr0 = Normal write-back */
	wsr("ttbr0_el1", 0);
	wsr("tcr_el1", 0);
	isb();

	/* ORGN0/IRGN0 = WBWA, SH0 = inner shareable, EPD1, IPS = 40-bit. */
	tcr |= (1UL << 10) | (1UL << 8) | (3UL << 12) | (1UL << 23) | (2UL << 32);

	if (sixteen) {
		for (int i = 0; i < 2048; i++)
			pt16k[i] = ((uint64_t)i << 25) | BLK_FLAGS | D_BLOCK;
		tcr |= (2UL << 14) | (2UL << 30);	/* TG0 = TG1 = 16KB */
		clean_dcache((uintptr_t)pt16k, (uintptr_t)(pt16k + 2048));
		wsr("ttbr0_el1", (uintptr_t)pt16k);
	} else {
		for (int i = 0; i < 512; i++)
			pt4k[i] = ((uint64_t)i << 30) | BLK_FLAGS | D_BLOCK;
		tcr |= (0UL << 14) | (2UL << 30);	/* TG0 = 4KB */
		clean_dcache((uintptr_t)pt4k, (uintptr_t)(pt4k + 512));
		wsr("ttbr0_el1", (uintptr_t)pt4k);
	}
	wsr("tcr_el1", tcr);
	isb();
	__asm__ volatile("dsb sy; tlbi vmalle1; dsb nsh; isb" ::: "memory");

	__asm__ volatile("at s1e1r, %0; isb" : : "r"(va) : "memory");
	par = rsr("par_el1");

	wsr("tcr_el1", 0);
	wsr("ttbr0_el1", 0);
	isb();
	return par;
}

/*
 * ID_AA64MMFR0 granule fields. The encodings differ between the 16K field and
 * the 4K/64K fields, which is a classic source of misreads:
 *   TGran4[19:16], TGran64[27:24]: 0b0000 supported, 0b1111 not implemented
 *   TGran16[23:20]:                0b0001 supported, 0b0000 not supported
 */
static int gran16_supported(uint64_t v)
{
	return ((v >> 20) & 0xf) == 1;
}

static void print_mmfr0(uint64_t v)
{
	con_hex("MMFR0", v);
	con_hex2("  PARange", "=", (uint32_t)(v & 0xf), "ASID=", (uint32_t)((v >> 4) & 0xf));
	con_hex2("  TGran4", "=", (uint32_t)((v >> 16) & 0xf),
		 "TGran16=", (uint32_t)((v >> 20) & 0xf));
	con_hex2("  TGran64", "=", (uint32_t)((v >> 24) & 0xf), 0, 0);
}

/* ---- FDT reporting ---- */

static void print_memory(uintptr_t fdt)
{
	uintptr_t m = fdt_node(fdt, "/memory");
	uint32_t len;
	const void *p;

	if (!m) {
		con_str("MEM", "no /memory node");
		return;
	}
	p = fdt_prop(fdt, m, "reg", &len);
	if (!p || len < 16) {
		con_str("MEM", "no reg");
		return;
	}
	/* Two address cells and two size cells: <hi lo hi lo>. */
	uint64_t base = fdt_be64(p);
	uint64_t size = fdt_be64((const uint8_t *)p + 8);
	con_hex2("MEM", "lo=", (uint32_t)base, "sz=", (uint32_t)size);
	con_hex2("MEM", "hi=", (uint32_t)(base >> 32), "szhi=", (uint32_t)(size >> 32));
}

static void print_reserved(uintptr_t fdt)
{
	uintptr_t r = fdt_node(fdt, "/reserved-memory");
	int n = 0, shown = 0;

	if (!r) {
		con_str("RESV", "no /reserved-memory");
		return;
	}
	for (uintptr_t c = fdt_first_child(fdt, r); c; c = fdt_next_sibling(fdt, c)) {
		uint32_t len;
		const void *p = fdt_prop(fdt, c, "reg", &len);

		if (!p || len < 16)
			continue;
		n++;
		if (shown < 3) {
			uint64_t base = fdt_be64(p);
			uint64_t size = fdt_be64((const uint8_t *)p + 8);
			con_hex2("RESV", "at=", (uint32_t)base, "sz=", (uint32_t)size);
			shown++;
		}
	}
	con_hex2("RESV", "count=", (uint32_t)n, "shown=", (uint32_t)shown);
}

static void print_gic_node(uintptr_t fdt)
{
	/* Qualcomm DTs put the GIC under /soc, but allow the root too. */
	uintptr_t g = fdt_node(fdt, "/soc/interrupt-controller");

	if (!g)
		g = fdt_node(fdt, "/interrupt-controller");
	uint32_t len;
	const void *p;

	if (!g) {
		con_str("GICNODE", "none");
		return;
	}
	p = fdt_prop(fdt, g, "compatible", &len);
	if (p)
		con_str("GICCOMPAT", (const char *)p);
	p = fdt_prop(fdt, g, "reg", &len);
	if (p && len >= 16)
		con_hex2("GICREG", "d=", (uint32_t)fdt_be64(p),
			 "c=", (uint32_t)fdt_be64((const uint8_t *)p + 8));
}

static void print_cpus(uintptr_t fdt)
{
	uintptr_t cpus = fdt_node(fdt, "/cpus");
	int n = 0;

	if (!cpus)
		return;
	for (uintptr_t c = fdt_first_child(fdt, cpus); c; c = fdt_next_sibling(fdt, c)) {
		uint32_t len;
		const void *p = fdt_prop(fdt, c, "device_type", &len);

		if (p && len >= 3 && ((const char *)p)[0] == 'c' &&
		    ((const char *)p)[1] == 'p' && ((const char *)p)[2] == 'u')
			n++;
	}
	con_hex2("CPUS", "count=", (uint32_t)n, 0, 0);
}

/*
 * The architected timers' PPI numbers differ per SoC and XNU hardcodes the
 * virtual timer's INTID. Print the four <type, number> triples so the board
 * config can be filled in from the live tree instead of from a guess.
 */
static void print_timer(uintptr_t fdt)
{
	uintptr_t t = fdt_node(fdt, "/soc/timer");
	uint32_t len;
	const void *p;
	int virt = -1;

	if (!t)
		t = fdt_node(fdt, "/timer");
	if (!t) {
		con_str("TIMER", "node not found");
		return;
	}
	p = fdt_prop(fdt, t, "interrupts", &len);
	if (!p || len < 12) {
		con_str("TIMER", "no interrupts");
		return;
	}
	/* Cells are <type number flags>; type 1 = PPI. Order is
	 * secure-phys, phys, virt, hyp, so the third entry is the virtual one. */
	for (unsigned int i = 0; i + 3 <= len; i += 3) {
		uint32_t type = fdt_be32((const uint8_t *)p + i * 4);
		uint32_t num = fdt_be32((const uint8_t *)p + i * 4 + 4);

		if (type == 1 && (i / 3) == 2)
			virt = (int)(num + 16);
	}
	if (virt >= 0)
		con_hex2("VTIMER", "ppi=", (uint32_t)(virt - 16), "intid=", (uint32_t)virt);
	else
		con_str("TIMER", "no PPI entries");
}

/* ---- GIC ---- */

static void print_gic(void)
{
	uintptr_t d = GICD_BASE;
	uint32_t iidr = rd32(d + GICD_IIDR);
	uint32_t typer = rd32(d + GICD_TYPER);
	uint32_t rev = (rd32(d + GICD_PIDR2) >> 4) & 0xf;

	con_hex2("GICD", "iidr=", iidr, "typer=", typer);
	con_hex2("GICREV", "arch=", rev, "ctlr=", rd32(d + GICD_CTLR));
	if (rev == 2)
		con_str("GICVER", "v2");
	else if (rev == 3)
		con_str("GICVER", "v3 (no GICC frame)");
	else
		con_str("GICVER", "unknown");
#if GIC_IS_V2
	con_hex2("GICC", "ctlr=", rd32(GICC_BASE + GICC_CTLR),
		 "hppir=", rd32(GICC_BASE + GICC_HPPIR));
#endif
}

/* ---- entry ---- */

void probe_main(uint64_t dtb, const char *loader_reason)
{
	uint64_t mmfr0 = rsr("id_aa64mmfr0_el1");
	int has16 = gran16_supported(mmfr0);

	con_clear(BLUE);
	/*
	 * Headline first: the screen holds ~38 rows and the probe prints more
	 * than that, so the answer that decides the port goes on top where it
	 * cannot scroll off.
	 */
	con_str("PROBE", BOARD_NAME " board probe");
	con_str("LOADER", loader_reason);   /* why the kernel was not booted */
	con_str("16K GRANULE", has16 ? "SUPPORTED" : "NOT SUPPORTED");

	/* Everything that cannot hang the CPU is printed before the AT tests. */
	con_hex2("MIDR", "=", (uint32_t)rsr("midr_el1"),
		 "MPIDR=", (uint32_t)rsr("mpidr_el1"));
	print_mmfr0(mmfr0);
	con_hex2("PFR0", "lo=", (uint32_t)rsr("id_aa64pfr0_el1"),
		 "isar0=", (uint32_t)rsr("id_aa64isar0_el1"));
	con_hex2("CNTFRQ", "=", (uint32_t)rsr("cntfrq_el0"), 0, 0);

	con_hex2("FDT", "at=", (uint32_t)dtb, "magic=", (uint32_t)fdt_be32((const void *)dtb));
	if (dtb) {
		uint32_t len;
		uintptr_t root = fdt_node(dtb, "/");
		const void *model = root ? fdt_prop(dtb, root, "model", &len) : 0;

		if (model)
			con_str("MODEL", (const char *)model);
		print_memory(dtb);
		print_cpus(dtb);
		print_gic_node(dtb);
		print_timer(dtb);
		print_reserved(dtb);
	}

	print_gic();

	/*
	 * 4K is mandatory on every ARMv8 CPU, so this AT validates the page
	 * table construction and the AT mechanism itself. If it fails, the
	 * 16K result below would not be trustworthy either.
	 */
	{
		uint64_t par4 = granule_test(0, (uintptr_t)probe_main);
		con_hex2("AT4K", "par=", (uint32_t)par4, "fail=", (uint32_t)(par4 & 1));
	}

	if (!has16) {
		/*
		 * Do NOT program an unsupported TG0: TCR_EL1 becomes UNKNOWN and
		 * the AT can hang the core, which on this phone means a battery
		 * pull. The ID register is architecturally authoritative.
		 */
		con_str("AT16K", "skipped (TG0 would be UNKNOWN)");
		con_str("VERDICT", "4K CPU: XNU needs mixed page sizes");
	} else {
		uint64_t par16 = granule_test(1, (uintptr_t)probe_main);
		con_hex2("AT16K", "par=", (uint32_t)par16, "fail=", (uint32_t)(par16 & 1));
		con_str("VERDICT", (par16 & 1) ? "ID says 16K, AT failed: check TCR"
					       : "16K granule usable");
	}

	con_str("PROBE", "done");
}
