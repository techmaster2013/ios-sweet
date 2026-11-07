#pragma once
#include <stdint.h>

struct loader_video {
	uint64_t base, width, height, stride;
};

/* A SWEETXNU payload in the initramfs the firmware (or QEMU -initrd) loaded? */
int loader_find(uintptr_t fdt, uint64_t *start, uint64_t *end);
/* Copy the kernel, place ADT + boot_args, jump. Returns only with an error. */
const char *loader_boot(uint64_t rd_start, uint64_t rd_end, const struct loader_video *video);
