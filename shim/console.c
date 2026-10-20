/*
 * Minimal text console on the ABL splash framebuffer. With the MMU off
 * every access is Device memory, so stay aligned (-mstrict-align).
 *
 * The panel format differs per board: sweet leaves a 32 bpp a8r8g8b8 splash
 * at 0x9c000000, ugglite a 24 bpp RGB888 one at 0x90001000. Pixel writes go
 * through con_put() so the 24 bpp path only touches three aligned bytes.
 */
#include "console.h"
#include "font.h"

#define SCALE		FB_SCALE
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

volatile uint32_t *con_fb = (volatile uint32_t *)FB_PHYS;
static uint32_t con_bg;
static int row;

/*
 * Write one pixel. The framebuffer is Device memory and we run with the MMU
 * off, so only naturally aligned accesses are used: for 24 bpp that is the
 * three bytes at fb + y*stride + x*3 (byte stores, always aligned to 1).
 */
static inline void con_put(int x, int y, uint32_t c)
{
	volatile uint8_t *p = (volatile uint8_t *)con_fb + (uintptr_t)y * FB_STRIDE + x * FB_BPP;

#if FB_BPP == 4
#if FB_SWAP_RB
	/* Colour constants are 0xAARRGGBB; this panel stores 0xAABBGGRR. */
	c = (c & 0xff00ff00U) | ((c & 0x00ff0000U) >> 16) | ((c & 0x000000ffU) << 16);
#endif
	*(volatile uint32_t *)p = c;
#else
	/* 0xRRGGBB in the low 24 bits; r8g8b8 means byte order R,G,B. */
	p[0] = (uint8_t)(c >> 16);
	p[1] = (uint8_t)(c >> 8);
	p[2] = (uint8_t)c;
#endif
}

void con_clear(uint32_t bg)
{
	for (int y = 0; y < FB_H; y++)
		for (int x = 0; x < FB_W; x++)
			con_put(x, y, bg);
	/* Mirror the background into every other frame the panel may scan out. */
	for (int f = 1; f < FB_FRAMES; f++) {
		volatile uint32_t *dst = (volatile uint32_t *)((volatile uint8_t *)con_fb
							       + (uintptr_t)f * FB_FRAME_BYTES);
		uint32_t c = bg;
#if FB_BPP == 4 && FB_SWAP_RB
		c = (c & 0xff00ff00U) | ((c & 0x00ff0000U) >> 16) | ((c & 0x000000ffU) << 16);
#endif
		for (int i = 0; i < FB_FRAME_BYTES / 4; i++)
			dst[i] = c;
	}
	con_bg = bg;
	row = 0;
}

/*
 * Draw one character into one frame. `base` is the frame's start; frame 0 is
 * con_fb itself.
 */
static void put_char_in(volatile uint8_t *base, int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;

	if (x0 + CH_W > FB_W || y0 + CH_H > FB_H)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
#if FB_BPP == 4 && FB_SWAP_RB
			c = (c & 0xff00ff00U) | ((c & 0x00ff0000U) >> 16) |
			    ((c & 0x000000ffU) << 16);
#endif
			for (int sy = 0; sy < SCALE; sy++)
				for (int sx = 0; sx < SCALE; sx++) {
					volatile uint8_t *p = base +
					    (uintptr_t)(y0 + gy * SCALE + sy) * FB_STRIDE +
					    (uintptr_t)(x0 + gx * SCALE + sx) * FB_BPP;
#if FB_BPP == 4
					*(volatile uint32_t *)p = c;
#else
					/* 24 bpp: three bytes, never a 32-bit store. */
					p[0] = (uint8_t)(c >> 16);
					p[1] = (uint8_t)(c >> 8);
					p[2] = (uint8_t)c;
#endif
				}
		}
}

/* Every glyph is drawn into all frames, so whichever one scans out shows it. */
static void put_char(int col, char ch, uint32_t fg)
{
	for (int f = 0; f < FB_FRAMES; f++)
		put_char_in((volatile uint8_t *)con_fb + (uintptr_t)f * FB_FRAME_BYTES,
			    col, ch, fg);
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_skip(void)
{
	row++;
}

void con_str(const char *label, const char *s)
{
	put_str(0, label, YELLOW);
	put_str(LABEL_COLS, s, WHITE);
	row++;
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	row++;
}
