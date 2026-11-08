#pragma once
#include <stdint.h>

/*
 * A found node: where its properties start, plus what is needed to turn
 * its "reg" into a CPU physical address (parent cells, parent "ranges"
 * and the grandparent's address cells). One level of translation, which
 * covers the Pi 5's soc@107c000000 bus directly under the root.
 */
struct fdt_node {
	uintptr_t props;
	int depth;
	uint32_t pac, psc;		/* parent #address-cells / #size-cells */
	uint32_t gpac;			/* grandparent #address-cells */
	uintptr_t pranges;		/* parent "ranges" value, 0 if none/empty */
	uint32_t pranges_len;
};

int fdt_valid(uintptr_t fdt);
/* Node whose "compatible" list contains compat (n-th match, from 0). */
int fdt_find_compatible(uintptr_t fdt, const char *compat, int nth, struct fdt_node *out);
/* Node by absolute path, e.g. "/soc@107c000000/serial@7d001000" or "/chosen". */
int fdt_find_path(uintptr_t fdt, const char *path, struct fdt_node *out);
/* Node whose name starts with prefix at the given depth (1 = child of root). */
int fdt_find_name(uintptr_t fdt, const char *prefix, int depth, int nth, struct fdt_node *out);
/* Node with this phandle. */
int fdt_find_phandle(uintptr_t fdt, uint32_t phandle, struct fdt_node *out);

/* Property value/length of a found node. */
int fdt_prop(uintptr_t fdt, const struct fdt_node *n, const char *name, uintptr_t *val, uint32_t *len);
int fdt_prop_u32(uintptr_t fdt, const struct fdt_node *n, const char *name, uint32_t *out);
/* idx-th (addr, size) of "reg", translated through the parent's "ranges". */
int fdt_reg(uintptr_t fdt, const struct fdt_node *n, int idx, uint64_t *addr, uint64_t *size);

uint32_t fdt_be32(uintptr_t a);
/* Read cells big-endian 32-bit cells (1 or 2) as one value. */
uint64_t fdt_cells(uintptr_t a, uint32_t cells);
int fdt_streq(const char *a, const char *b);
