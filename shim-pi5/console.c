/*
 * Output for the Pi 5 shim: PL011 debug UART (the 3-pin JST header) and
 * the firmware's simple-framebuffer on HDMI. MMU off = Device memory, so
 * all accesses stay naturally aligned (-mstrict-align).
 */
#include <stdarg.h>
#include "console.h"
#include "../shim/font.h"

#define UART_DR		0x00
#define UART_FR		0x18
#define UART_IBRD	0x24
#define UART_FBRD	0x28
#define UART_LCRH	0x2c
#define UART_CR		0x30
#define UART_IMSC	0x38
#define UART_ICR	0x44
#define FR_TXFF		(1U << 5)
#define FR_BUSY		(1U << 3)
#define CR_UARTEN	(1U << 0)
#define CR_TXE		(1U << 8)
#define CR_RXE		(1U << 9)
#define LCRH_FEN	(1U << 4)
#define LCRH_WLEN8	(3U << 5)

#define MARGIN		16

static uintptr_t uart;
static uintptr_t fb;
static uint32_t fb_w, fb_h, fb_stride, fb_bpp, scale;
static uint32_t fg = C_WHITE, bg = C_BG;
static uint32_t col, row;

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

void uart_init(uintptr_t base, uint32_t clk_hz, uint32_t baud)
{
	uart = base;
	if (!base)
		return;
	if ((rd32(base + UART_CR) & (CR_UARTEN | CR_TXE)) == (CR_UARTEN | CR_TXE))
		return;	/* firmware (uart_2ndstage=1) already set it up */
	if (!clk_hz)
		clk_hz = 44236800;	/* bcm2712 clk_uart; /16/115200 = 24 exactly */
	while (rd32(base + UART_FR) & FR_BUSY)
		;
	wr32(base + UART_CR, 0);
	/* divisor = clk / (16 * baud), as 16.6 fixed point */
	uint32_t div64 = (uint32_t)(((uint64_t)clk_hz * 4 + baud / 2) / baud);
	wr32(base + UART_IBRD, div64 >> 6);
	wr32(base + UART_FBRD, div64 & 63);
	wr32(base + UART_LCRH, LCRH_WLEN8 | LCRH_FEN);
	wr32(base + UART_IMSC, 0);
	wr32(base + UART_ICR, 0x7ff);
	wr32(base + UART_CR, CR_UARTEN | CR_TXE | CR_RXE);
}

static void uart_putc(char c)
{
	if (!uart)
		return;
	if (c == '\n')
		uart_putc('\r');
	while (rd32(uart + UART_FR) & FR_TXFF)
		;
	wr32(uart + UART_DR, (uint8_t)c);
}

static uint32_t pack(uint32_t argb)
{
	if (fb_bpp == 16)
		return ((argb >> 8) & 0xf800) | ((argb >> 5) & 0x07e0) | ((argb >> 3) & 0x001f);
	return argb;
}

static void px(uint32_t x, uint32_t y, uint32_t c)
{
	uintptr_t a = fb + y * fb_stride;
	if (fb_bpp == 16)
		*(volatile uint16_t *)(a + 2 * x) = (uint16_t)c;
	else
		*(volatile uint32_t *)(a + 4 * x) = c;
}

void fb_init(uintptr_t base, uint32_t w, uint32_t h, uint32_t stride, uint32_t bpp)
{
	if (!base || !w || !h || (bpp != 16 && bpp != 32))
		return;
	fb = base;
	fb_w = w;
	fb_h = h;
	fb_stride = stride ? stride : w * bpp / 8;
	fb_bpp = bpp;
	scale = w >= 2560 ? 3 : w >= 1280 ? 2 : 1;
}

void fb_clear(uint32_t argb)
{
	bg = argb;
	col = row = 0;
	if (!fb)
		return;
	uint32_t c = pack(argb);
	for (uint32_t y = 0; y < fb_h; y++)
		for (uint32_t x = 0; x < fb_w; x++)
			px(x, y, c);
}

void con_color(uint32_t argb)
{
	fg = argb;
}

static void fb_putc(char ch)
{
	if (!fb)
		return;
	uint32_t cw = 8 * scale, chh = 16 * scale;
	uint32_t cols = (fb_w - 2 * MARGIN) / cw, rows = (fb_h - 2 * MARGIN) / chh;
	if (ch == '\n') {
		col = 0;
		row++;
		return;
	}
	if (col >= cols) {
		col = 0;
		row++;
	}
	if (row >= rows)
		return;	/* screen full: UART still has everything */
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	uint32_t f = pack(fg), b = pack(bg);
	uint32_t x0 = MARGIN + col * cw, y0 = MARGIN + row * chh;
	for (uint32_t gy = 0; gy < 16; gy++)
		for (uint32_t gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? f : b;
			for (uint32_t sy = 0; sy < scale; sy++)
				for (uint32_t sx = 0; sx < scale; sx++)
					px(x0 + gx * scale + sx, y0 + gy * scale + sy, c);
		}
	col++;
}

static void out(char c)
{
	uart_putc(c);
	fb_putc(c);
}

static void out_num(uint64_t v, unsigned base, int width, int zero, int neg)
{
	char buf[24];
	int n = 0;
	do {
		unsigned d = v % base;
		buf[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		v /= base;
	} while (v);
	if (neg)
		buf[n++] = '-';
	while (n < width)
		buf[n++] = zero ? '0' : ' ';
	while (n)
		out(buf[--n]);
}

void pr(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			out(*fmt);
			continue;
		}
		fmt++;
		int zero = 0, width = 0, lng = 0;
		if (*fmt == '0')
			zero = 1, fmt++;
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		while (*fmt == 'l')
			lng++, fmt++;
		switch (*fmt) {
		case 's': {
			const char *s = va_arg(ap, const char *);
			while (s && *s)
				out(*s++);
			break;
		}
		case 'c':
			out((char)va_arg(ap, int));
			break;
		case 'd': {
			int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
			out_num(v < 0 ? -(uint64_t)v : (uint64_t)v, 10, width, zero, v < 0);
			break;
		}
		case 'u':
			out_num(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, width, zero, 0);
			break;
		case 'x':
			out_num(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, width, zero, 0);
			break;
		case 'p':
			out('0');
			out('x');
			out_num((uintptr_t)va_arg(ap, void *), 16, 0, 0, 0);
			break;
		case '%':
			out('%');
			break;
		default:
			out('?');
			break;
		}
	}
	va_end(ap);
}
