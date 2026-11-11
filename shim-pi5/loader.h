#pragma once
#include <stdint.h>

struct loader_video {
	uint64_t base, width, height, stride;
};

/* Per-board facts from the firmware DT, patched into the ADT at boot. */
struct loader_board {
	const char *serial;	/* /serial-number, or 0 */
	uint64_t ram_top;	/* end of the RAM region holding the kernel, or 0 */
	uint32_t soc_rev;	/* 0xc0 / 0xd0 from the firmware DT's pinctrl, or 0 */
};

/* A SWEETXNU payload in the initramfs the firmware (or QEMU -initrd) loaded? */
int loader_find(uintptr_t fdt, uint64_t *start, uint64_t *end);
/* Copy the kernel, place ADT + boot_args, jump. Returns only with an error. */
const char *loader_boot(uint64_t rd_start, uint64_t rd_end, const struct loader_video *video,
			const struct loader_board *board);
