#pragma once
#include <stdint.h>
#include "board.h"

#ifndef FB_STRIDE_BYTES
#define FB_STRIDE_BYTES	(FB_W * FB_BPP)
#endif
#define FB_STRIDE	FB_STRIDE_BYTES	/* bytes per line */
#ifndef FB_FRAMES
#define FB_FRAMES	1
#endif
#ifndef FB_FRAME_BYTES
#define FB_FRAME_BYTES	(FB_STRIDE_BYTES * FB_H)
#endif
#ifndef FB_SWAP_RB
#define FB_SWAP_RB	0
#endif

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
