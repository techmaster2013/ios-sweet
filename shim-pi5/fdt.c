/*
 * Flattened device tree reader for the Pi 5 shim. Runs with the MMU off,
 * so every load is a Device access: only aligned 32-bit and byte loads.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define MAX_DEPTH	16

uint32_t fdt_be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

uint64_t fdt_cells(uintptr_t a, uint32_t cells)
{
	uint64_t v = 0;
	for (uint32_t i = 0; i < cells; i++)
		v = v << 32 | fdt_be32(a + 4 * i);
	return v;
}

static char ch(uintptr_t p)
{
	return *(volatile char *)p;
}

int fdt_streq(const char *a, const char *b)
{
	while (ch((uintptr_t)a) && ch((uintptr_t)a) == ch((uintptr_t)b))
		a++, b++;
	return ch((uintptr_t)a) == ch((uintptr_t)b);
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	while (ch(p))
		p++;
	return p + 1;
}

int fdt_valid(uintptr_t fdt)
{
	return fdt && (fdt & 3) == 0 && fdt_be32(fdt) == FDT_MAGIC;
}

enum match { M_COMPAT, M_PATH, M_NAME, M_PHANDLE };

struct query {
	enum match kind;
	const char *s;
	uint32_t num;		/* phandle, or depth for M_NAME */
	int nth;
};

/* "compatible" is a list of NUL-terminated strings. */
static int compat_has(uintptr_t val, uint32_t len, const char *want)
{
	uintptr_t end = val + len;
	while (val < end) {
		if (fdt_streq((const char *)val, want))
			return 1;
		val = skip_str(val);
	}
	return 0;
}

static int prefix(const char *s, const char *p)
{
	while (ch((uintptr_t)p))
		if (ch((uintptr_t)s++) != ch((uintptr_t)p++))
			return 0;
	return 1;
}

/* Does the node stack (names[1..depth]) spell path exactly? */
static int path_is(const uintptr_t *names, int depth, const char *path)
{
	const char *p = path;
	if (ch((uintptr_t)p) == '/' && ch((uintptr_t)p + 1) == 0)
		return depth == 0;
	for (int d = 1; d <= depth; d++) {
		if (ch((uintptr_t)p) != '/')
			return 0;
		p++;
		const char *n = (const char *)names[d];
		while (ch((uintptr_t)n) && ch((uintptr_t)n) == ch((uintptr_t)p))
			n++, p++;
		/* "name" in the path may omit "@unit" */
		if (ch((uintptr_t)n) && ch((uintptr_t)n) != '@')
			return 0;
		char c = ch((uintptr_t)p);
		if (c != '/' && c != 0)
			return 0;
	}
	return ch((uintptr_t)p) == 0;
}

static int search(uintptr_t fdt, const struct query *q, struct fdt_node *out)
{
	if (!fdt_valid(fdt))
		return 0;
	uintptr_t p = fdt + fdt_be32(fdt + 8);
	uintptr_t strings = fdt + fdt_be32(fdt + 12);
	uint32_t acells[MAX_DEPTH], scells[MAX_DEPTH], rlen[MAX_DEPTH];
	uintptr_t ranges[MAX_DEPTH], names[MAX_DEPTH], props[MAX_DEPTH];
	int depth = -1, seen = 0;

	for (;;) {
		uint32_t tok = fdt_be32(p);
		p += 4;
		if (tok == FDT_BEGIN_NODE) {
			if (++depth >= MAX_DEPTH)
				return 0;
			names[depth] = p;
			acells[depth] = 2;
			scells[depth] = 1;
			ranges[depth] = 0;
			rlen[depth] = 0;
			p = align4(skip_str(p));
			props[depth] = p;
			int hit = 0;
			if (q->kind == M_PATH)
				hit = path_is(names, depth, q->s);
			else if (q->kind == M_NAME)
				hit = depth == (int)q->num && prefix((const char *)names[depth], q->s);
			if (hit && seen++ == q->nth)
				goto found;
		} else if (tok == FDT_END_NODE) {
			if (--depth < -1)
				return 0;
		} else if (tok == FDT_PROP) {
			uint32_t len = fdt_be32(p), nameoff = fdt_be32(p + 4);
			uintptr_t val = p + 8;
			const char *name = (const char *)(strings + nameoff);
			if (depth >= 0) {
				if (fdt_streq(name, "#address-cells"))
					acells[depth] = fdt_be32(val);
				else if (fdt_streq(name, "#size-cells"))
					scells[depth] = fdt_be32(val);
				else if (fdt_streq(name, "ranges")) {
					ranges[depth] = len ? val : 0;
					rlen[depth] = len;
				}
				int hit = 0;
				if (q->kind == M_COMPAT && fdt_streq(name, "compatible"))
					hit = compat_has(val, len, q->s);
				else if (q->kind == M_PHANDLE && fdt_streq(name, "phandle") && len == 4)
					hit = fdt_be32(val) == q->num;
				if (hit && seen++ == q->nth)
					goto found;
			}
			p = align4(val + len);
		} else if (tok != FDT_NOP) {
			return 0;	/* FDT_END or garbage */
		}
	}
found:
	out->props = props[depth];
	out->depth = depth;
	out->pac = depth >= 1 ? acells[depth - 1] : 2;
	out->psc = depth >= 1 ? scells[depth - 1] : 1;
	out->gpac = depth >= 2 ? acells[depth - 2] : 2;
	out->pranges = depth >= 2 ? ranges[depth - 1] : 0;
	out->pranges_len = depth >= 2 ? rlen[depth - 1] : 0;
	return 1;
}

int fdt_find_compatible(uintptr_t fdt, const char *compat, int nth, struct fdt_node *out)
{
	struct query q = { M_COMPAT, compat, 0, nth };
	return search(fdt, &q, out);
}

int fdt_find_path(uintptr_t fdt, const char *path, struct fdt_node *out)
{
	struct query q = { M_PATH, path, 0, 0 };
	return search(fdt, &q, out);
}

int fdt_find_name(uintptr_t fdt, const char *pfx, int depth, int nth, struct fdt_node *out)
{
	struct query q = { M_NAME, pfx, (uint32_t)depth, nth };
	return search(fdt, &q, out);
}

int fdt_find_phandle(uintptr_t fdt, uint32_t phandle, struct fdt_node *out)
{
	struct query q = { M_PHANDLE, 0, phandle, 0 };
	return search(fdt, &q, out);
}

int fdt_prop(uintptr_t fdt, const struct fdt_node *n, const char *name, uintptr_t *val, uint32_t *len)
{
	uintptr_t strings = fdt + fdt_be32(fdt + 12);
	uintptr_t p = n->props;
	for (;;) {
		uint32_t tok = fdt_be32(p);
		p += 4;
		if (tok == FDT_NOP)
			continue;
		if (tok != FDT_PROP)
			return 0;	/* properties come before subnodes */
		uint32_t l = fdt_be32(p), nameoff = fdt_be32(p + 4);
		if (fdt_streq((const char *)(strings + nameoff), name)) {
			*val = p + 8;
			*len = l;
			return 1;
		}
		p = align4(p + 8 + l);
	}
}

int fdt_prop_u32(uintptr_t fdt, const struct fdt_node *n, const char *name, uint32_t *out)
{
	uintptr_t v;
	uint32_t l;
	if (!fdt_prop(fdt, n, name, &v, &l) || l < 4)
		return 0;
	*out = fdt_be32(v);
	return 1;
}

int fdt_reg(uintptr_t fdt, const struct fdt_node *n, int idx, uint64_t *addr, uint64_t *size)
{
	uintptr_t v;
	uint32_t l;
	uint32_t stride = 4 * (n->pac + n->psc);
	if (!fdt_prop(fdt, n, "reg", &v, &l) || stride == 0 || (uint32_t)(idx + 1) * stride > l)
		return 0;
	v += idx * stride;
	uint64_t a = fdt_cells(v, n->pac);
	*size = fdt_cells(v + 4 * n->pac, n->psc);

	/* Parent bus "ranges": <child-addr parent-addr size>, child in pac cells. */
	if (n->pranges) {
		uint32_t rs = 4 * (n->pac + n->gpac + n->psc);
		for (uint32_t off = 0; rs && off + rs <= n->pranges_len; off += rs) {
			uintptr_t r = n->pranges + off;
			uint64_t child = fdt_cells(r, n->pac);
			uint64_t parent = fdt_cells(r + 4 * n->pac, n->gpac);
			uint64_t len = fdt_cells(r + 4 * (n->pac + n->gpac), n->psc);
			if (a >= child && a - child < len) {
				a = a - child + parent;
				break;
			}
		}
	}
	*addr = a;
	return 1;
}
