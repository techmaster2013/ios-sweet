#pragma once
#include <stdint.h>

#define C_BG		0xff101830U	/* dark blue: shim alive */
#define C_PANIC		0xff0000a0U
#define C_WHITE		0xffffffffU
#define C_YELLOW	0xffffff00U
#define C_GREEN		0xff00ff00U
#define C_RED		0xffff4040U

/* PL011 at base. Keeps the firmware setup if the UART is already on; else programs clk_hz (0 = bcm2712 default). */
void uart_init(uintptr_t base, uint32_t clk_hz, uint32_t baud);
/* fb at base; bpp 32 (a8r8g8b8/x8r8g8b8) or 16 (r5g6b5). */
void fb_init(uintptr_t base, uint32_t w, uint32_t h, uint32_t stride, uint32_t bpp);
void fb_clear(uint32_t argb);
void con_color(uint32_t argb);

/* printf subset to UART + screen: %s %c %d %u %x %lx %llx %p, width/zero-pad on numbers. */
void pr(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
