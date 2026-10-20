#pragma once
/*
 * Per-board physical constants for the shim.
 *
 * Select at build time with -DBOARD_UGGLITE (default: sweet, the Redmi Note
 * 10 Pro / SM7150). Everything the shim touches before it loads a payload is
 * here: the ABL splash framebuffer, the interrupt controller and the two
 * Qualcomm restart registers.
 *
 * Sources for the ugglite values: the mainline device tree
 * (msm8917-xiaomi-ugglite.dts in the postmarketOS tree) and the stock MIUI
 * DTB #07 ("MSM8917-PMI8937 MTP", board-id <8 0>), which agree.
 */

#if defined(BOARD_UGGLITE)

#define BOARD_NAME	"ugglite"

/*
 * reserved-memory splash_region. The mainline DTS puts the simple-framebuffer
 * at 0x90001000; the stock DTB has splash_region@83000000 at 0x90000000 with
 * size 0x1400000. If the console comes up blank on hardware, rebuild with
 * -DFB_PHYS=0x90000000.
 */
#ifndef FB_PHYS
#define FB_PHYS		0x90001000UL
#endif
#define FB_PHYS_ALT	0x90000000UL
#define FB_W		720
#define FB_H		1280
#define FB_BPP		3		/* RGB888, mainline format "r8g8b8" */
#define FB_SCALE	2		/* console glyph scale */

/* qcom,msm-qgic2: GICv2, distributor + CPU interface in two frames. */
#define GICD_BASE	0x0b000000UL
#define GICC_BASE	0x0b002000UL
#define GIC_IS_V2	1

/*
 * Timer PPI numbering. Both boards list the four architected timers as
 * secure-phys, phys, virt, hyp, but the PPIs differ:
 *   sweet   (msm8917-style list not used): <PPI 1,2,3,0> -> virt = INTID 19
 *   ugglite (stock dtb_07 and mainline agree): <PPI 2,3,4,1> -> virt = INTID 20
 * The probe tests VTIMER_INTID and reports it, because XNU has to know.
 */
#define VTIMER_INTID	20
#define PTIMER_INTID	19
#define SGI_INTID	1

/* restart@4ab000 (qcom,pshold) and msm-imem restart_reason@65c. */
#define PSHOLD_PHYS		0x004ab000UL
#define IMEM_RESTART_REASON	0x0860065cUL
#define IMEM_BASE		0x08600000UL

/* TLMM for the button/panel GPIOs (probe only reports it). */
#define TLMM_BASE	0x01000000UL

#else /* sweet, the default board */

#define BOARD_NAME	"sweet"

#define FB_PHYS		0x9c000000UL
#define FB_PHYS_ALT	0x9c000000UL
#define FB_W		1080
#define FB_H		2400
#define FB_BPP		4		/* a8r8g8b8 */
#define FB_SCALE	3

#define GICD_BASE	0x17a00000UL
#define GICC_BASE	0x0UL		/* GICv3: redistributors, no GICC frame */
#define GIC_IS_V2	0

#define PSHOLD_PHYS		0x0c264000UL
#define IMEM_RESTART_REASON	0x146aa65cUL
#define IMEM_BASE		0x146a8000UL

#define TLMM_BASE	0x03100000UL

/* sweet keeps the Qualcomm PPI numbering where virt = PPI 3 = INTID 19. */
#define VTIMER_INTID	19
#define PTIMER_INTID	18
#define SGI_INTID	1

#endif
