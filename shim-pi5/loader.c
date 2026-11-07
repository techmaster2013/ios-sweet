/*
 * Stage 2: act as iBoot for the SWEET xnu built for the Pi 5.
 *
 * Same payload as the phone (tools/mkpayload.py with an ADT from
 * tools/mkadt_pi5.py): the Pi firmware loads it as the initramfs
 * (config.txt "initramfs payload.bin followkernel"), QEMU with -initrd.
 * Copy the kernel segments to phys_base + (vmaddr - virt_base), put the
 * ADT and boot_args behind the kernel, and jump to the entry point with
 * the MMU and caches off, x0 = boot_args (physical), at EL1.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const volatile char *m)
{
	const char want[8] = { 'S', 'W', 'E', 'E', 'T', 'X', 'N', 'U' };
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

static int chosen_addr(uintptr_t fdt, const char *prop, uint64_t *out)
{
	struct fdt_node n;
	uintptr_t v;
	uint32_t l;
	if (!fdt_find_path(fdt, "/chosen", &n) || !fdt_prop(fdt, &n, prop, &v, &l) || (l != 4 && l != 8))
		return 0;
	*out = fdt_cells(v, l / 4);
	return 1;
}

int loader_find(uintptr_t fdt, uint64_t *start, uint64_t *end)
{
	if (!chosen_addr(fdt, "linux,initrd-start", start) || !chosen_addr(fdt, "linux,initrd-end", end))
		return 0;
	return (*start & 7) == 0 && *end > *start + 0x2a8 && magic_ok((const volatile char *)*start);
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

const char *loader_boot(uint64_t rd_start, uint64_t rd_end, const struct loader_video *video)
{
	const volatile struct payload *p = (const volatile struct payload *)rd_start;
	if (p->version != 1 || (p->adt_off & 7))
		return "bad payload header";

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	pr("payload %lx-%lx: %u segs, phys %lx + %lx, virt %lx, entry %lx\n",
	   rd_start, rd_end, p->nsegs, p->phys_base, p->mem_size, p->virt_base, p->entry_va);

	/* The payload, this shim and the DTB must not sit where the kernel goes. */
	extern char _head[], _end[];
	uint64_t lo = p->phys_base, hi = p->phys_base + p->mem_size;
	if (rd_end > lo && rd_start < hi)
		return "payload overlaps kernel memory (move phys_base or the initramfs)";
	if ((uintptr_t)_end > lo && (uintptr_t)_head < hi)
		return "shim overlaps kernel memory";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const volatile struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		if (s->file_off + s->file_size > rd_end - rd_start)
			return "segment past end of payload";
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	if (video && video->base) {
		ba->Video.v_baseAddr = video->base;
		ba->Video.v_display = 1;
		ba->Video.v_rowBytes = video->stride;
		ba->Video.v_width = video->width;
		ba->Video.v_height = video->height;
		ba->Video.v_depth = 32;
	}
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	pr("ADT %lx (%lu bytes), boot_args %lx, top %lx\n", (uint64_t)adt_pa, p->adt_size,
	   (uint64_t)ba_pa, (uint64_t)top);
	pr("jumping to xnu at %lx\n", (uint64_t)entry_pa);

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
