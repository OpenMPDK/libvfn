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

#define log_fmt(fmt) "iommu/iommufd: " fmt

#include <stdint.h>
#include <limits.h>

#include <linux/iommufd.h>

#include "vfn/trace.h"
#include "vfn/support.h"
#include "vfn/pci.h"
#include "vfn/iommu.h"

#include "ccan/compiler/compiler.h"
#include "ccan/list/list.h"

#include "context.h"
#include "trace.h"

static int __iommufd = -1;

struct iommu_ioas {
	struct iommu_ctx ctx;

	char *name;
	uint32_t id;
};

static struct iommu_ioas iommufd_default_ioas = {
	.name = "default",
};

static int iommu_ioas_update_iova_ranges(struct iommu_ioas *ioas)
{
	struct iommu_ioas_iova_ranges iova_ranges = {
		.size = sizeof(iova_ranges),
		.ioas_id = ioas->id,
		.num_iovas = 0,
	};

	if (ioctl(__iommufd, IOMMU_IOAS_IOVA_RANGES, &iova_ranges)) {
		if (errno != EMSGSIZE) {
			log_debug("could not get ioas iova ranges\n");
			return -1;
		}

		ioas->ctx.nranges = iova_ranges.num_iovas;
		ioas->ctx.iova_ranges = reallocn(ioas->ctx.iova_ranges, iova_ranges.num_iovas,
						 sizeof(struct iommu_iova_range));

		iova_ranges.allowed_iovas = (uintptr_t)ioas->ctx.iova_ranges;

		if (ioctl(__iommufd, IOMMU_IOAS_IOVA_RANGES, &iova_ranges)) {
			log_debug("could not get ioas iova ranges\n");
			return -1;
		}
	}

	if (logv(LOG_INFO)) {
		for (int i = 0; i < ioas->ctx.nranges; i++) {
			struct iommu_iova_range *r = &ioas->ctx.iova_ranges[i];
			__autofree char *str = NULL;

			log_fatal_if(iommu_iova_range_to_string(r, &str) < 0,
				     "iommu_iova_range_to_string\n");

			log_info("iova range %d is %s\n", i, str);
		}
	}

	return 0;
}

static int iommufd_get_device_fd(struct iommu_ctx *ctx, const char *bdf)
{
	struct iommu_ioas *ioas = container_of_var(ctx, ioas, ctx);

	__autofree char *vfio_id = NULL;
	__autofree char *path = NULL;
	int devfd;

	struct vfio_device_bind_iommufd bind = {
		.argsz = sizeof(bind),
		.flags = 0,
		.iommufd = __iommufd,
	};

	struct vfio_device_attach_iommufd_pt attach_data = {
		.argsz = sizeof(attach_data),
		.flags = 0,
		.pt_id = ioas->id,
	};

	vfio_id = pci_get_device_vfio_id(bdf);
	if (!vfio_id) {
		log_debug("could not determine the vfio device id for %s\n", bdf);
		return -1;
	}

	if (asprintf(&path, "/dev/vfio/devices/%s", vfio_id) < 0) {
		log_debug("asprintf failed\n");
		return -1;
	}

	devfd = open(path, O_RDWR);
	if (devfd < 0) {
		log_debug("could not open the device cdev\n");
		return -1;
	}

	if (ioctl(devfd, VFIO_DEVICE_BIND_IOMMUFD, &bind)) {
		log_debug("could not bind device to iommufd\n");
		goto close_dev;
	}

	if (ioctl(devfd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach_data)) {
		log_debug("could not associate device with ioas\n");
		goto close_dev;
	}

	if (iommu_ioas_update_iova_ranges(ioas)) {
		log_debug("could not update iova ranges\n");
		goto close_dev;
	}

	return devfd;

close_dev:
	/* closing devfd will automatically unbind it from iommufd */
	log_fatal_if(close(devfd), "close: %s\n", strerror(errno));

	return -1;
}

static int iommu_ioas_do_dma_map(struct iommu_ctx *ctx, struct iova_mapping *m)
{
	struct iommu_ioas *ioas = container_of_var(ctx, ioas, ctx);

	struct iommu_ioas_map map = {
		.size = sizeof(map),
		.flags = IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE,
		.ioas_id = ioas->id,
		.user_va = (uint64_t)m->vaddr,
		.length = m->len,
	};

	if (m->flags & IOMMU_MAP_FIXED_IOVA) {
		map.flags |= IOMMU_IOAS_MAP_FIXED_IOVA;
		map.iova = m->iova;
	}

	if (m->flags & IOMMU_MAP_NOWRITE)
		map.flags &= ~IOMMU_IOAS_MAP_WRITEABLE;

	if (m->flags & IOMMU_MAP_NOREAD)
		map.flags &= ~IOMMU_IOAS_MAP_READABLE;

	trace_guard(IOMMUFD_IOAS_MAP_DMA) {
		if (m->flags & IOMMU_MAP_FIXED_IOVA)
			trace_emit("vaddr %p iova 0x%" PRIx64 " len %zu\n", m->vaddr, m->iova,
				m->len);
		else
			trace_emit("vaddr %p iova AUTO len %zu\n", m->vaddr, m->len);
	}

	if (ioctl(__iommufd, IOMMU_IOAS_MAP, &map)) {
		log_debug("failed to map\n");
		return -1;
	}

	if (m->flags & IOMMU_MAP_FIXED_IOVA)
		return 0;

	m->iova = map.iova;

	trace_guard(IOMMUFD_IOAS_MAP_DMA) {
		trace_emit("allocated iova 0x%" PRIx64 "\n", m->iova);
	}

	return 0;
}

static int iommu_ioas_do_dma_unmap(struct iommu_ctx *ctx, struct iova_mapping *m)
{
	struct iommu_ioas *ioas = container_of_var(ctx, ioas, ctx);
	uint64_t iova = m->iova;
	size_t len = m->len;

	struct iommu_ioas_unmap unmap = {
		.size = sizeof(unmap),
		.ioas_id = ioas->id,
		.iova = iova,
		.length = len
	};

	trace_guard(IOMMUFD_IOAS_UNMAP_DMA) {
		trace_emit("iova 0x%" PRIx64 " len %zu\n", iova, len);
	}

	if (ioctl(__iommufd, IOMMU_IOAS_UNMAP, &unmap)) {
		log_debug("failed to unmap\n");
		return -1;
	}

	return 0;
}

static int iommu_ioas_do_dma_unmap_all(struct iommu_ctx *ctx)
{
	struct iova_mapping m = {
		.iova = 0,
		.len = UINT64_MAX
	};
	return iommu_ioas_do_dma_unmap(ctx, &m);
}

static const struct iommu_ctx_ops iommufd_ops = {
	.get_device_fd = iommufd_get_device_fd,

	.dma_map = iommu_ioas_do_dma_map,
	.dma_unmap = iommu_ioas_do_dma_unmap,
	.dma_unmap_all = iommu_ioas_do_dma_unmap_all,
};

static int iommu_ioas_init(struct iommu_ioas *ioas)
{
	struct iommu_ioas_alloc alloc_data = {
		.size = sizeof(alloc_data),
		.flags = 0,
	};

	if (ioctl(__iommufd, IOMMU_IOAS_ALLOC, &alloc_data)) {
		log_debug("could not allocate ioas\n");
		return -1;
	}

	ioas->id = alloc_data.out_ioas_id;

	memcpy(&ioas->ctx.ops, &iommufd_ops, sizeof(ioas->ctx.ops));

	return 0;
}

static int iommufd_open(void)
{
	__iommufd = open("/dev/iommu", O_RDWR);
	if (__iommufd < 0)
		return -1;

	return 0;
}

struct iommu_ctx *iommufd_get_iommu_context(const char *name)
{
	struct iommu_ioas *ioas = znew_t(struct iommu_ioas, 1);

	iommu_ctx_init(&ioas->ctx);

	if (__iommufd == -1)
		log_fatal_if(iommufd_open(), "could not open /dev/iommu\n");

	if (iommu_ioas_init(ioas) < 0) {
		free(ioas);
		return NULL;
	}

	ioas->name = strdup(name);

	return &ioas->ctx;
}

struct iommu_ctx *iommufd_get_default_iommu_context(void)
{
	if (__iommufd == -1) {
		iommu_ctx_init(&iommufd_default_ioas.ctx);

		log_fatal_if(iommufd_open(), "could not open /dev/iommu\n");
		log_fatal_if(iommu_ioas_init(&iommufd_default_ioas), "iommu_ioas_init\n");
	}

	return &iommufd_default_ioas.ctx;
}
