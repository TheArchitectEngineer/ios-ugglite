/*
 * Just enough flattened-device-tree parsing to find what ABL put in
 * /chosen (initrd location) and, for the board probe, the live memory map.
 *
 * Big-endian, 4-byte aligned tokens; the blob is Device memory while the MMU
 * is off, so every access is a byte or aligned word load.
 *
 * A node handle is the byte offset of its FDT_BEGIN_NODE token. 0 means
 * "none" (the root's token is never at offset 0 because the header is first).
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

static uintptr_t struct_off(uintptr_t fdt) { return fdt + be32(fdt + 8); }
static uintptr_t strings_off(uintptr_t fdt) { return fdt + be32(fdt + 12); }

static int valid(uintptr_t fdt)
{
	return fdt && be32(fdt) == FDT_MAGIC;
}

/* First token after a node's BEGIN_NODE token and name. */
static uintptr_t node_body(uintptr_t tok)
{
	return align4(skip_str(tok + 4));
}

/* Offset just past the subtree rooted at `tok` (which points at BEGIN_NODE). */
static uintptr_t skip_subtree(uintptr_t tok)
{
	uintptr_t p = tok + 4;
	int depth = 1;

	p = align4(skip_str(p));
	while (depth > 0) {
		uint32_t t = be32(p);
		p += 4;
		switch (t) {
		case FDT_BEGIN_NODE:
			p = align4(skip_str(p));
			depth++;
			break;
		case FDT_END_NODE:
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p);
			p = align4(p + 8 + len);
			break;
		}
		case FDT_NOP:
			break;
		default:
			return p;
		}
	}
	return p;
}

/* Does the node's name equal `seg` (length seglen) or start with "seg@"? */
static int name_matches(const char *name, const char *seg, int seglen)
{
	int i = 0;
	while (i < seglen && name[i] == seg[i])
		i++;
	return i == seglen && (name[i] == 0 || name[i] == '@');
}

/* First child of `tok` whose name matches, 0 if none. */
static uintptr_t find_child(uintptr_t tok, const char *seg, int seglen)
{
	uintptr_t p = node_body(tok);

	for (;;) {
		uint32_t t = be32(p);
		if (t == FDT_END_NODE)
			return 0;
		if (t == FDT_BEGIN_NODE) {
			if (name_matches((const char *)(p + 4), seg, seglen))
				return p;
			p = skip_subtree(p);
			continue;
		}
		p += 4;
		if (t == FDT_PROP) {
			uint32_t len = be32(p);
			p = align4(p + 8 + len);
		} else if (t != FDT_NOP) {
			return 0;
		}
	}
}

/* ---- /chosen property lookup (the loader's original use) ---- */

struct chosen_ctx {
	const char *prop;
	uint64_t val;
	int found;
};

static int chosen_fn(uintptr_t fdt, uintptr_t tok, int depth, uintptr_t name, void *vctx)
{
	struct chosen_ctx *c = vctx;

	if (depth != 2 || !streq((const char *)name, "chosen"))
		return 0;
	/* Scan this node's properties until its END_NODE. */
	for (uintptr_t p = node_body(tok);;) {
		uint32_t t = be32(p);
		p += 4;
		if (t == FDT_END_NODE)
			return 1;
		if (t != FDT_PROP) {
			if (t == FDT_NOP)
				continue;
			return 1;
		}
		uint32_t len = be32(p), nameoff = be32(p + 4);
		uintptr_t val = p + 8;
		if (streq((const char *)(strings_off(fdt) + nameoff), c->prop)) {
			if (len == 4)
				c->val = be32(val);
			else if (len == 8)
				c->val = (uint64_t)be32(val) << 32 | be32(val + 4);
			else
				return 1;
			c->found = 1;
			return 1;
		}
		p = align4(val + len);
	}
}

typedef int (*node_fn)(uintptr_t fdt, uintptr_t tok, int depth, uintptr_t name,
		       void *ctx);

static int walk(uintptr_t fdt, node_fn fn, void *ctx)
{
	uintptr_t p = struct_off(fdt);
	int depth = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			uintptr_t name = p;
			p = align4(skip_str(p));
			depth++;
			if (fn(fdt, p, depth, name, ctx))
				return 1;
			break;
		}
		case FDT_END_NODE:
			depth--;
			if (depth < 0)
				return 0;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p);
			p = align4(p + 8 + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	struct chosen_ctx c = { prop, 0, 0 };

	if (!valid(fdt))
		return 0;
	walk(fdt, chosen_fn, &c);
	if (c.found)
		*out = c.val;
	return c.found;
}

/* ---- generic navigation ---- */

uintptr_t fdt_node(uintptr_t fdt, const char *path)
{
	if (!valid(fdt))
		return 0;

	/* The root node's BEGIN_NODE token is the first thing in the block. */
	uintptr_t tok = struct_off(fdt);
	const char *seg = path[0] == '/' ? path + 1 : path;

	while (*seg) {
		const char *end = seg;
		while (*end && *end != '/')
			end++;
		tok = find_child(tok, seg, (int)(end - seg));
		if (!tok)
			return 0;
		seg = *end ? end + 1 : end;
	}
	return tok;
}

const char *fdt_node_name(uintptr_t node)
{
	return (const char *)(node + 4);
}

const void *fdt_prop(uintptr_t fdt, uintptr_t node, const char *name, uint32_t *len)
{
	for (uintptr_t p = node_body(node);;) {
		uint32_t t = be32(p);
		p += 4;
		if (t == FDT_END_NODE || t == FDT_BEGIN_NODE)
			return 0;	/* no more properties in this node */
		if (t == FDT_NOP)
			continue;
		if (t != FDT_PROP)
			return 0;
		uint32_t l = be32(p), nameoff = be32(p + 4);
		uintptr_t val = p + 8;
		if (streq((const char *)(strings_off(fdt) + nameoff), name)) {
			if (len)
				*len = l;
			return (const void *)val;
		}
		p = align4(val + l);
	}
}

uintptr_t fdt_first_child(uintptr_t fdt, uintptr_t node)
{
	uintptr_t p = node_body(node);

	for (;;) {
		uint32_t t = be32(p);
		if (t == FDT_BEGIN_NODE)
			return p;
		if (t == FDT_END_NODE)
			return 0;
		p += 4;
		if (t == FDT_PROP) {
			uint32_t len = be32(p);
			p = align4(p + 8 + len);
		} else if (t != FDT_NOP) {
			return 0;
		}
	}
	(void)fdt;
}

uintptr_t fdt_next_sibling(uintptr_t fdt __attribute__((unused)), uintptr_t node)
{
	uintptr_t p = skip_subtree(node);
	return be32(p) == FDT_BEGIN_NODE ? p : 0;
}

uint32_t fdt_be32(const void *p)
{
	const volatile uint8_t *b = p;
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

uint64_t fdt_be64(const void *p)
{
	return (uint64_t)fdt_be32(p) << 32 | fdt_be32((const uint8_t *)p + 4);
}
