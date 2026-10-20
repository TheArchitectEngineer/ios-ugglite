/*
 * Print the framebuffer's physical address and geometry from the running
 * system, using raw syscalls so it can be built static and run on Android.
 *
 *   clang --target=aarch64-linux-gnu -static -nostdlib -O2 fbinfo.c -o fbinfo
 *
 * The MSM fb driver does not expose smem_start through sysfs, so this asks
 * the driver directly with FBIOGET_FSCREENINFO / FBIOGET_VSCREENINFO.
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define SYS_write 64
#define SYS_openat 56
#define SYS_ioctl 29
#define SYS_exit 93
#define AT_FDCWD -100
#define O_RDONLY 0

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

struct fb_fix_screeninfo {
	char id[16];
	u64 smem_start;
	u32 smem_len;
	u32 type;
	u32 type_aux;
	u32 visual;
	u16 xpanstep;
	u16 ypanstep;
	u16 ywrapstep;
	u32 line_length;
	u64 mmio_start;
	u32 mmio_len;
	u32 accel;
	u16 capabilities;
	u16 reserved[2];
};

struct fb_bitfield {
	u32 offset;
	u32 length;
	u32 msb_right;
};

struct fb_var_screeninfo {
	u32 xres, yres;
	u32 xres_virtual, yres_virtual;
	u32 xoffset, yoffset;
	u32 bits_per_pixel, grayscale;
	struct fb_bitfield red, green, blue, transp;
	u32 nonstd, activate, height, width;
	u32 accel_flags;
	u32 pixclock, left_margin, right_margin, upper_margin, lower_margin;
	u32 hsync_len, vsync_len, sync, vmode, rotate, colorspace;
	u32 reserved[4];
};

static long sys(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory");
	return x0;
}

static void put(const char *s)
{
	long n = 0;
	while (s[n])
		n++;
	sys(SYS_write, 1, (long)s, n, 0, 0, 0);
}

static void puthex(u64 v)
{
	char b[19];
	int i = 18;
	b[18] = 0;
	b[17] = '\n';
	for (i = 16; i >= 0; i--) {
		u64 d = v & 0xf;
		b[i] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
		v >>= 4;
	}
	sys(SYS_write, 1, (long)b, 19, 0, 0, 0);
}

static void putdec(long v)
{
	char b[24];
	int i = 23;
	b[23] = '\n';
	if (v == 0) {
		b[22] = '0';
		sys(SYS_write, 1, (long)(b + 22), 2, 0, 0, 0);
		return;
	}
	while (v > 0 && i > 0) {
		b[--i] = (char)('0' + v % 10);
		v /= 10;
	}
	sys(SYS_write, 1, (long)(b + i), 24 - i, 0, 0, 0);
}

static void label(const char *s) { put(s); put(": "); }

void _start(void)
{
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	long fd = sys(SYS_openat, AT_FDCWD, (long)"/dev/graphics/fb0", O_RDONLY, 0, 0, 0);

	if (fd < 0) {
		put("open /dev/graphics/fb0 failed\n");
		sys(SYS_exit, 1, 0, 0, 0, 0, 0);
	}
	if (sys(SYS_ioctl, fd, FBIOGET_FSCREENINFO, (long)&fix, 0, 0, 0) < 0) {
		put("FBIOGET_FSCREENINFO failed\n");
		sys(SYS_exit, 1, 0, 0, 0, 0, 0);
	}
	if (sys(SYS_ioctl, fd, FBIOGET_VSCREENINFO, (long)&var, 0, 0, 0) < 0) {
		put("FBIOGET_VSCREENINFO failed\n");
		sys(SYS_exit, 1, 0, 0, 0, 0, 0);
	}

	label("id");
	put(fix.id);
	put("\n");

	label("smem_start");
	puthex(fix.smem_start);
	label("smem_len");
	puthex(fix.smem_len);
	label("line_length");
	putdec(fix.line_length);

	label("xres");
	putdec(var.xres);
	label("yres");
	putdec(var.yres);
	label("xres_virtual");
	putdec(var.xres_virtual);
	label("yres_virtual");
	putdec(var.yres_virtual);
	label("bits_per_pixel");
	putdec(var.bits_per_pixel);

	label("red");
	putdec(var.red.offset);
	put(" len ");
	putdec(var.red.length);
	label("green");
	putdec(var.green.offset);
	label("blue");
	putdec(var.blue.offset);
	label("transp");
	putdec(var.transp.offset);

	sys(SYS_exit, 0, 0, 0, 0, 0, 0);
}
