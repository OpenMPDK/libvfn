// SPDX-License-Identifier: LGPL-2.1-or-later or MIT

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2023 The libvfn Authors. All Rights Reserved.
 *
 * This library (libvfn) is dual licensed under the GNU Lesser General
 * Public License version 2.1 or later or the MIT license. See the
 * COPYING and LICENSE files for more information.
 */

#define log_fmt(fmt) "iommu/dma: " fmt

#ifdef __cplusplus
extern "C" {
#endif

#include "ccan/compiler/compiler.h"
#include "ccan/minmax/minmax.h"

#include "vfn/iommu.h"
#include "vfn/support.h"

#include "context.h"

static int iova_cmp(const void *vaddr, const struct skiplist_node *n)
{
	struct iova_mapping *m = container_of_var(n, m, list);

	if (vaddr < m->vaddr)
		return -1;
	else if ((uintptr_t)vaddr >= ((uintptr_t)m->vaddr) + m->len)
		return 1;

	return 0;
}

static int iova_map_add(struct iova_map *map, struct iova_mapping *m)
{
	__autolock(&map->lock);

	struct skiplist_node *update[SKIPLIST_LEVELS] = {};

	if (!m->len) {
		errno = EINVAL;
		return -1;
	}

	if (skiplist_find(&map->list, m->vaddr, iova_cmp, update)) {
		errno = EEXIST;
		return -1;
	}

	skiplist_link(&map->list, &m->list, update);

	return 0;
}

static void iova_map_remove(struct iova_map *map, void *vaddr)
{
	__autolock(&map->lock);

	struct skiplist_node *n, *update[SKIPLIST_LEVELS] = {};

	n = skiplist_find(&map->list, vaddr, iova_cmp, update);
	if (!n)
		return;

	skiplist_erase(&map->list, n, update);
}

static struct iova_mapping *iova_map_find(struct iova_map *map, void *vaddr)
{
	__autolock(&map->lock);

	return container_of_or_null(skiplist_find(&map->list, vaddr, iova_cmp, NULL),
				    struct iova_mapping, list);
}

static void iova_map_clear_with(struct iova_map *map, skiplist_iter_fn fn, void *opaque)
{
	__autolock(&map->lock);

	skiplist_clear_with(&map->list, fn, opaque);
}

static void UNUSED iova_map_clear(struct iova_map *map)
{
	iova_map_clear_with(map, NULL, NULL);
}

bool iommu_translate_vaddr(struct iommu_ctx *ctx, void *vaddr, uint64_t *iova)
{
	struct iova_mapping *m = iova_map_find(&ctx->map, vaddr);

	if (m) {
		*iova = m->iova + ((uintptr_t)vaddr - (uintptr_t)m->vaddr);
		return true;
	}

	return false;
}

int _iommu_map_vaddr(struct iommu_ctx *ctx, void *vaddr, size_t len, uint64_t *iova,
		    unsigned long flags, void *opaque)
{
	uint64_t _iova;
	struct iova_mapping *m;
	if (iommu_translate_vaddr(ctx, vaddr, &_iova))
		goto out;

	if (flags & IOMMU_MAP_FIXED_IOVA) {
		_iova = *iova;
	} else if (ctx->ops.iova_reserve && ctx->ops.iova_reserve(ctx, len, &_iova, flags)) {
		log_debug("failed to allocate iova\n");
		return -1;
	}

	m = znew_t(struct iova_mapping, 1);
	m->vaddr = vaddr;
	m->len = len;
	m->iova = 0; // Physical/IOMMU address
	m->opaque[0] = NULL; // IODMACommand
	m->opaque[1] = opaque; // IOMemoryDescriptor
	m->flags = flags;

	if (ctx->ops.dma_map(ctx, m)) {
		log_error("failed to map dma\n");
		free(m);
		return -1;
	}

	if (iova_map_add(&ctx->map, m)) {
		log_error("failed to add mapping\n");
		free(m);
		return -1;
	}

	_iova = m->iova;
out:
	if (iova)
		*iova = _iova;

	return 0;
}

#ifndef __APPLE__
int iommu_map_vaddr(struct iommu_ctx *ctx, void *vaddr, size_t len, uint64_t *iova,
		    unsigned long flags)
{
	return _iommu_map_vaddr(ctx, vaddr, len, iova, flags, NULL);
}
#endif

int iommu_unmap_vaddr(struct iommu_ctx *ctx, void *vaddr, size_t *len)
{
	struct iova_mapping *m;

	m = iova_map_find(&ctx->map, vaddr);
	if (!m) {
		errno = ENOENT;
		return -1;
	}

	if (len)
		*len = m->len;

	if (ctx->ops.dma_unmap(ctx, m)) {
		log_debug("failed to unmap dma\n");
		return -1;
	}

	if (m->flags & IOMMU_MAP_EPHEMERAL && ctx->ops.iova_put_ephemeral)
		ctx->ops.iova_put_ephemeral(ctx);

	iova_map_remove(&ctx->map, m->vaddr);

	return 0;
}

static void __unmap_mapping(void *opaque, struct skiplist_node *n)
{
	struct iommu_ctx *ctx = (struct iommu_ctx *)opaque;
	struct iova_mapping *m = container_of_var(n, m, list);

	log_fatal_if(ctx->ops.dma_unmap(ctx, m),
		     "failed to unmap dma (iova 0x%" PRIx64 " len %zu)\n", m->iova, m->len);

	free(m);
}

int iommu_unmap_all(struct iommu_ctx *ctx)
{
	if (ctx->ops.dma_unmap_all) {
		if (ctx->ops.dma_unmap_all(ctx)) {
			log_debug("failed to unmap dma\n");
			return -1;
		}

		iova_map_clear(&ctx->map);

		return 0;
	}

	iova_map_clear_with(&ctx->map, __unmap_mapping, ctx);

	return 0;
}

#ifndef __APPLE__
int iommu_get_iova_ranges(struct iommu_ctx *ctx, struct iommu_iova_range **ranges)
{
	*ranges = ctx->iova_ranges;
	return ctx->nranges;
}

int iommu_iova_range_to_string(struct iommu_iova_range *r, char **str)
{
	return asprintf(str, "[0x%llx; 0x%llx]", r->start, r->last);
}
#endif

#ifdef __cplusplus
}
#endif
