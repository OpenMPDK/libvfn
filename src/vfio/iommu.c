// SPDX-License-Identifier: LGPL-2.1-or-later or MIT

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2022 The libvfn Authors. All Rights Reserved.
 *
 * This library (libvfn) is dual licensed under the GNU Lesser General
 * Public License version 2.1 or later or the MIT license. See the
 * COPYING and LICENSE files for more information.
 */

#define log_fmt(fmt) "vfio/iommu: " fmt

#include <assert.h>
#include <byteswap.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/vfio.h>

#include <vfn/vfio/container.h>

#include <vfn/support/align.h>
#include <vfn/support/atomic.h>
#include <vfn/support/autoptr.h>
#include <vfn/support/compiler.h>
#include <vfn/support/log.h>
#include <vfn/support/mem.h>
#include <vfn/support/mutex.h>

#include <vfn/trace.h>

#include "ccan/compiler/compiler.h"
#include "ccan/list/list.h"
#include "ccan/minmax/minmax.h"

#include "iommu.h"

#define SKIPLIST_LEVELS 8

#define skip_list_top(h, k) \
	list_top(&(h)->heads[k], struct iova_map_entry, list[k])
#define skip_list_next(h, n, k) \
	list_next(&(h)->heads[k], (n), list[k])
#define skip_list_add_after(h, p, n, k) \
	list_add_after(&(h)->heads[k], &(p)->list[k], &(n)->list[k])
#define skip_list_add(h, n, k) \
	list_add(&(h)->heads[k], &(n)->list[k])
#define skip_list_del_from(h, n, k) \
	list_del_from(&(h)->heads[k], &(n)->list[k])
#define skip_list_for_each_safe(h, n, next) \
	list_for_each_safe(&(h)->heads[0], n, next, list[0])

struct iova_map_entry {
	struct iova_mapping mapping;
	struct list_node list[SKIPLIST_LEVELS];
};

struct iova_map {
	pthread_mutex_t lock;

	int height;

	struct iova_map_entry nil, sentinel;
	struct list_head heads[SKIPLIST_LEVELS];
};

static void iova_map_init(struct iova_map *map)
{
	pthread_mutex_init(&map->lock, NULL);

	map->height = 0;
	map->nil.mapping.vaddr = (void *)UINT64_MAX;

	for (int k = 0; k < SKIPLIST_LEVELS; k++) {
		list_head_init(&map->heads[k]);

		skip_list_add(map, &map->nil, k);
		skip_list_add(map, &map->sentinel, k);
	}
}

static void iova_map_clear_with(struct iova_map *map, iova_mapping_iter_fn fn, void *opaque)
{
	__autolock(&map->lock);

	struct iova_map_entry *n, *next;

	skip_list_for_each_safe(map, n, next) {
		skip_list_del_from(map, n, 0);

		if (n == &map->nil && n == &map->sentinel)
			continue;

		if (fn)
			fn(opaque, &n->mapping);

		free(n);
	}
}

static struct iova_map_entry *__iova_map_find_path(struct iova_map *map, void *vaddr,
						   struct iova_map_entry **path)
{
	struct iova_map_entry *next, *p = &map->sentinel;
	int k = map->height;

	do {
		next = skip_list_next(map, p, k);
		while (vaddr >= next->mapping.vaddr + next->mapping.len) {
			p = next;
			next = skip_list_next(map, p, k);
		}

		if (path)
			path[k] = p;
	} while (--k >= 0);

	p = skip_list_next(map, p, 0);

	if (vaddr >= p->mapping.vaddr && vaddr < p->mapping.vaddr + p->mapping.len)
		return p;

	return NULL;
}

static struct iova_map_entry *iova_map_find_path(struct iova_map *map, void *vaddr,
						 struct iova_map_entry **path)
{
	__autolock(&map->lock);

	return __iova_map_find_path(map, vaddr, path);
}

struct iova_mapping *iommu_find_mapping(struct iommu_state *iommu, void *vaddr)
{
	struct iova_map_entry *n;

	n = iova_map_find_path(iommu->map, vaddr, NULL);
	if (!n)
		return NULL;

	return &n->mapping;
}

static inline int random_level(void)
{
	int k = 0;

	while (k < SKIPLIST_LEVELS - 1 && (rand() > (RAND_MAX / 2)))
		k++;

	return k;
}

static void __iova_map_add(struct iova_map *map, struct iova_map_entry *n,
			   struct iova_map_entry *update[SKIPLIST_LEVELS])
{
	int k;

	k = random_level();
	if (k > map->height) {
		k = ++(map->height);
		update[k] = &map->sentinel;
	}

	do {
		skip_list_add_after(map, update[k], n, k);
	} while (--k >= 0);
}

static int iova_map_add(struct iova_map *map, void *vaddr, size_t len, uint64_t iova)
{
	__autolock(&map->lock);

	struct iova_map_entry *n, *update[SKIPLIST_LEVELS] = {};

	if (__iova_map_find_path(map, vaddr, update)) {
		errno = EEXIST;
		return -1;
	}

	n = znew_t(struct iova_map_entry, 1);
	if (!n) {
		errno = ENOMEM;
		return -1;
	}

	n->mapping.vaddr = vaddr;
	n->mapping.len = len;
	n->mapping.iova = iova;

	__iova_map_add(map, n, update);

	return 0;
}

static void __iova_map_remove(struct iova_map *map, struct iova_map_entry *p,
			      struct iova_map_entry *update[SKIPLIST_LEVELS])
{
	struct iova_map_entry *next;

	for (int r = 0; r <= map->height; r++) {
		next = skip_list_next(map, update[r], r);
		if (next != p)
			break;

		skip_list_del_from(map, p, r);
	}

	free(p);

	/* reduce height if possible */
	while (map->height && skip_list_next(map, &map->sentinel, map->height) == &map->nil)
		map->height--;
}

static int iova_map_remove(struct iova_map *map, void *vaddr)
{
	__autolock(&map->lock);

	struct iova_map_entry *p, *update[SKIPLIST_LEVELS] = {};

	p = __iova_map_find_path(map, vaddr, update);
	if (!p) {
		errno = ENOENT;
		return -1;
	}

	__iova_map_remove(map, p, update);

	return 0;
}

void iommu_init(struct iommu_state *iommu)
{
	iommu->map = znew_t(struct iova_map, 1);

	iova_map_init(iommu->map);

	pthread_mutex_init(&iommu->lock, NULL);

	iommu->nranges = 1;
	iommu->next = __VFN_IOVA_MIN;

	iommu->iova_ranges = znew_t(struct vfio_iova_range, 1);
	iommu->iova_ranges[0].start = __VFN_IOVA_MIN;
	iommu->iova_ranges[0].end = IOVA_MAX_39BITS - 1;
}

void iommu_clear_with(struct iommu_state *iommu, iova_mapping_iter_fn fn, void *opaque)
{
	iova_map_clear_with(iommu->map, fn, opaque);
}

void iommu_clear(struct iommu_state *iommu)
{
	iova_map_clear_with(iommu->map, NULL, NULL);
}

void iommu_destroy(struct iommu_state *iommu)
{
	iommu_clear(iommu);

	free(iommu->map);
	free(iommu->iova_ranges);

	memset(iommu, 0x0, sizeof(*iommu));
}

int iommu_add_mapping(struct iommu_state *iommu, void *vaddr, size_t len, uint64_t iova)
{
	if (!len) {
		errno = EINVAL;
		return -1;
	}

	if (iova_map_add(iommu->map, vaddr, len, iova))
		return -1;

	return 0;
}

void iommu_remove_mapping(struct iommu_state *iommu, void *vaddr)
{
	iova_map_remove(iommu->map, vaddr);
}

int iommu_get_iova(struct iommu_state *iommu, size_t len, uint64_t *iova)
{
	__autolock(&iommu->lock);

	if (!ALIGNED(len, __VFN_PAGESIZE)) {
		log_debug("len is not page aligned\n");
		errno = EINVAL;
		return -1;
	}

	for (int i = 0; i < iommu->nranges; i++) {
		struct vfio_iova_range *r = &iommu->iova_ranges[i];
		uint64_t next = iommu->next;

		if (r->end < next)
			continue;

		next = max_t(uint64_t, next, r->start);

		if (next > r->end || r->end - next + 1 < len)
			continue;

		iommu->next = next + len;

		*iova = next;

		return 0;
	}

	errno = ENOMEM;
	return -1;
}

bool iommu_vaddr_to_iova(struct iommu_state *iommu, void *vaddr, uint64_t *iova)
{
	struct iova_mapping *m = iommu_find_mapping(iommu, vaddr);

	if (m) {
		*iova = m->iova + (vaddr - m->vaddr);
		return true;
	}

	return false;
}
