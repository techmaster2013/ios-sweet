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

/*
 * Apple device tree editing, in place (same-size values only). Layout
 * (tools/adt.py): node = u32 nprops, u32 nchildren, props, children;
 * prop = char name[32], u32 len (bit 31 a flag), value padded to 4 bytes.
 * The MMU is off (all memory is Device): only aligned 32-bit and byte accesses.
 */
static uint32_t adt_rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static void adt_wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static int adt_streq(uintptr_t a, const char *s, int max)
{
	int i;
	for (i = 0; i < max && s[i]; i++)
		if (*(volatile const char *)(a + i) != s[i])
			return 0;
	return i == max || *(volatile const char *)(a + i) == 0;
}

static uintptr_t adt_skip(uintptr_t n)
{
	uint32_t np = adt_rd(n), nc = adt_rd(n + 4);
	uintptr_t p = n + 8;
	for (uint32_t i = 0; i < np; i++)
		p += 36 + (((adt_rd(p + 32) & 0x7fffffff) + 3) & ~3u);
	for (uint32_t i = 0; i < nc; i++)
		p = adt_skip(p);
	return p;
}

static int adt_prop(uintptr_t n, const char *name, uintptr_t *val, uint32_t *len)
{
	uint32_t np = adt_rd(n);
	uintptr_t p = n + 8;
	for (uint32_t i = 0; i < np; i++) {
		uint32_t l = adt_rd(p + 32) & 0x7fffffff;
		if (adt_streq(p, name, 32)) {
			*val = p + 36;
			*len = l;
			return 1;
		}
		p += 36 + ((l + 3) & ~3u);
	}
	return 0;
}

/* Child of n whose "name" property is the first namelen chars of name. */
static uintptr_t adt_child(uintptr_t n, const char *name, int namelen)
{
	uint32_t np = adt_rd(n), nc = adt_rd(n + 4);
	uintptr_t p = n + 8;
	for (uint32_t i = 0; i < np; i++)
		p += 36 + (((adt_rd(p + 32) & 0x7fffffff) + 3) & ~3u);
	for (uint32_t i = 0; i < nc; i++) {
		uintptr_t v;
		uint32_t l;
		if (adt_prop(p, "name", &v, &l) && (int)l >= namelen) {
			int ok = 1;
			for (int k = 0; k < namelen && ok; k++)
				ok = *(volatile const char *)(v + k) == name[k];
			if (ok && ((int)l == namelen || *(volatile const char *)(v + namelen) == 0))
				return p;
		}
		p = adt_skip(p);
	}
	return 0;
}

/* "/a/b" -> node, or 0. */
static uintptr_t adt_path(uintptr_t root, const char *path)
{
	uintptr_t n = root;
	while (n && *path == '/') {
		path++;
		int l = 0;
		while (path[l] && path[l] != '/')
			l++;
		if (l)
			n = adt_child(n, path, l);
		path += l;
	}
	return n;
}

/* Overwrite a u32 or u64 property (value 4-byte aligned). */
static int adt_set(uintptr_t root, const char *path, const char *name, int idx, uint64_t v, int size)
{
	uintptr_t n = adt_path(root, path), val;
	uint32_t len;
	if (!n || !adt_prop(n, name, &val, &len) || (uint32_t)(idx + 1) * size > len)
		return 0;
	val += (uintptr_t)idx * size;
	adt_wr(val, (uint32_t)v);
	if (size == 8)
		adt_wr(val + 4, (uint32_t)(v >> 32));
	return 1;
}

/*
 * Per-board values (see tools/mkadt_pi5.py): serial number, RAM size for
 * boards with less RAM than the ADT assumes, the firmware framebuffer.
 * Returns the kernel-managed RAM size to put in boot_args.
 */
static uint64_t adt_patch(uintptr_t adt, uint64_t phys_base, uint64_t mem_size,
			  const struct loader_video *video, const struct loader_board *board)
{
	uintptr_t n, val;
	uint32_t len;

	if (board && board->serial && (n = adt_path(adt, "/")) && adt_prop(n, "serial-number", &val, &len) && len) {
		uint32_t i;
		for (i = 0; i + 1 < len && board->serial[i]; i++)
			*(volatile char *)(val + i) = board->serial[i];
		for (; i < len; i++)
			*(volatile char *)(val + i) = 0;
		pr("ADT serial-number %s\n", board->serial);
	}

	/* dram-size = mem_size; the panic log (/pram) sits right above it. */
	uintptr_t pram = adt_path(adt, "/pram");
	uint64_t panic_size = 0;
	if (pram && adt_prop(pram, "reg", &val, &len) && len >= 16)
		panic_size = adt_rd(val + 8) | (uint64_t)adt_rd(val + 12) << 32;
	if (board && board->ram_top && panic_size && phys_base + mem_size + panic_size > board->ram_top) {
		uint64_t total = (board->ram_top - phys_base) & ~((16UL << 20) - 1);
		mem_size = total - panic_size;
		adt_set(adt, "/chosen", "dram-size", 0, mem_size, 8);
		adt_set(adt, "/pram", "reg", 0, phys_base + mem_size, 8);
		pr("ADT RAM lowered to %lu MiB (board RAM ends at %lx)\n", mem_size >> 20, board->ram_top);
	}

	if (video && video->base) {
		uint64_t fbsize = (video->stride * video->height + 0x3fff) & ~0x3fffUL;
		adt_set(adt, "/sweet-fbcon", "reg", 0, video->base, 8);
		adt_set(adt, "/sweet-fbcon", "reg", 1, fbsize, 8);
		adt_set(adt, "/sweet-fbcon", "width", 0, video->width, 4);
		adt_set(adt, "/sweet-fbcon", "height", 0, video->height, 4);
		adt_set(adt, "/sweet-fbcon", "stride", 0, video->stride, 4);
		adt_set(adt, "/arm-io/disp0", "canvas-width", 0, video->width, 4);
		adt_set(adt, "/arm-io/disp0", "canvas-height", 0, video->height, 4);
		pr("ADT display %lux%lu at %lx\n", video->width, video->height, video->base);
	}
	return mem_size;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

const char *loader_boot(uint64_t rd_start, uint64_t rd_end, const struct loader_video *video,
			const struct loader_board *board)
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
	uint64_t mem_size = adt_patch(adt_pa, p->phys_base, p->mem_size, video, board);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = mem_size;
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
	ba->memSizeActual = mem_size;

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	pr("ADT %lx (%lu bytes), boot_args %lx, top %lx\n", (uint64_t)adt_pa, p->adt_size,
	   (uint64_t)ba_pa, (uint64_t)top);
	pr("jumping to xnu at %lx\n", (uint64_t)entry_pa);

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
