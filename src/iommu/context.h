/* SPDX-License-Identifier: LGPL-2.1-or-later or MIT */

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2023 The libvfn Authors. All Rights Reserved.
 *
 * This library (libvfn) is dual licensed under the GNU Lesser General
 * Public License version 2.1 or later or the MIT license. See the
 * COPYING and LICENSE files for more information.
 */

#ifndef LIBVFN_SRC_IOMMU_CONTEXT_H
#define LIBVFN_SRC_IOMMU_CONTEXT_H

#include "util/skiplist.h"

struct iommu_ctx;

struct iova_mapping {
	void *vaddr;
	size_t len;
	uint64_t iova;
	void *opaque[2];

	unsigned long flags;

	struct skiplist_node list;
};

struct iommu_ctx_ops {
	/* container/ioas ops */
	int (*iova_reserve)(struct iommu_ctx *ctx, size_t len, uint64_t *iova,
			    unsigned long flags);
	void (*iova_put_ephemeral)(struct iommu_ctx *ctx);
	int (*dma_map)(struct iommu_ctx *ctx, struct iova_mapping *m);
	int (*dma_unmap)(struct iommu_ctx *ctx, struct iova_mapping *m);
	int (*dma_unmap_all)(struct iommu_ctx *ctx);

	/* device ops */
	int (*get_device_fd)(struct iommu_ctx *ctx, const char *bdf);
};

struct iova_map {
	pthread_mutex_t lock;
	struct skiplist list;
};

struct iommu_ctx {
	struct iova_map map;
	struct iommu_ctx_ops ops;

	pthread_mutex_t lock;
	int nranges;
	struct iommu_iova_range *iova_ranges;
};

struct iommu_ctx *iommu_get_default_context(void);

struct iommu_ctx *vfio_get_default_iommu_context(void);
struct iommu_ctx *vfio_get_iommu_context(const char *name);

#ifdef HAVE_VFIO_DEVICE_BIND_IOMMUFD
struct iommu_ctx *iommufd_get_default_iommu_context(void);
struct iommu_ctx *iommufd_get_iommu_context(const char *name);
#endif

void iommu_ctx_init(struct iommu_ctx *ctx);
int iommu_iova_range_to_string(struct iommu_iova_range *range, char **str);

#endif /* LIBVFN_SRC_IOMMU_CONTEXT_H */
