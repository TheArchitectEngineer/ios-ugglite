#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/*
 * Read-only walker over a flattened device tree. Node handles are byte
 * offsets into the blob; 0 means "none". All values are big-endian and
 * unaligned-safe (the blob sits in Device memory when the MMU is off).
 */

/* Node at an absolute path ("/", "/memory", "/reserved-memory"). 0 if absent. */
uintptr_t fdt_node(uintptr_t fdt, const char *path);

/* Name of a node ("memory@80000000"), pointing into the blob. */
const char *fdt_node_name(uintptr_t node);

/* Property value (big-endian bytes), or 0. *len gets the length. */
const void *fdt_prop(uintptr_t fdt, uintptr_t node, const char *name, uint32_t *len);

/* First child / next sibling, 0 at the end. */
uintptr_t fdt_first_child(uintptr_t fdt, uintptr_t node);
uintptr_t fdt_next_sibling(uintptr_t fdt, uintptr_t node);

/* Convenience: big-endian cell reads. */
uint32_t fdt_be32(const void *p);
uint64_t fdt_be64(const void *p);
