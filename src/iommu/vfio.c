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

#define log_fmt(fmt) "iommu/vfio: " fmt

#include <sys/ioctl.h>
#include "ccan/str/str.h"
#include "ccan/compiler/compiler.h"
#include "ccan/minmax/minmax.h"

#include "vfn/support.h"
#include "vfn/iommu.h"
#include "vfn/trace.h"
#include "vfn/pci/util.h"

#include "context.h"

#define VFIO_IOMMU_TYPE1_IOVA_RESERVED 0x10000

struct vfio_group {
	int fd;
	struct vfio_container *container;

	char *path;
};

struct vfio_container {
	struct iommu_ctx ctx;
	char *name;

	int fd;

#define VFN_MAX_VFIO_GROUPS 64
	struct vfio_group groups[VFN_MAX_VFIO_GROUPS];

	pthread_mutex_t lock;
	uint64_t next, next_ephemeral, nephemerals;
	struct iommu_iova_range ephemerals;

	bool iommu_set;
};

static struct vfio_container vfio_default_container = {
	.fd = -1,
	.name = "default",
};

#ifdef VFIO_IOMMU_INFO_CAPS
# ifdef VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE
__static_assert(sizeof(struct vfio_iova_range) == sizeof(struct iommu_iova_range));

static void vfio_iommu_type1_get_cap_iova_ranges(struct iommu_ctx *ctx,
						 struct vfio_info_cap_header *cap)
{
	struct vfio_iommu_type1_info_cap_iova_range *cap_iova_range;
	size_t len;

	cap_iova_range = (struct vfio_iommu_type1_info_cap_iova_range *)cap;

	len = __abort_on_overflow(cap_iova_range->nr_iovas, sizeof(struct iommu_iova_range));

	ctx->nranges = cap_iova_range->nr_iovas;
	ctx->iova_ranges = realloc(ctx->iova_ranges, len);

	memcpy(ctx->iova_ranges, cap_iova_range->iova_ranges, len);

	if (logv(LOG_INFO)) {
		for (int i = 0; i < ctx->nranges; i++) {
			struct iommu_iova_range *r = &ctx->iova_ranges[i];
			__autofree char *str = NULL;

			log_fatal_if(iommu_iova_range_to_string(r, &str) < 0,
				     "iommu_iova_range_to_string\n");

			log_info("iova range %d is %s\n", i, str);
		}
	}
}
# endif

# ifdef VFIO_IOMMU_TYPE1_INFO_CAP_DMA_AVAIL
static void vfio_iommu_type1_get_cap_dma_avail(struct vfio_info_cap_header *cap)
{
	struct vfio_iommu_type1_info_dma_avail *dma;

	dma = (struct vfio_iommu_type1_info_dma_avail *)cap;

	log_info("dma avail %"PRIu32"\n", dma->avail);
}
# endif

static struct vfio_iommu_type1_info *vfio_iommu_type1_get_iommu_info(struct vfio_container *vfio)
{
	struct vfio_iommu_type1_info *iommu_info = NULL;
	uint32_t iommu_info_size = sizeof(*iommu_info);

	iommu_info = xmalloc(iommu_info_size);

	memset(iommu_info, 0x0, iommu_info_size);
	iommu_info->argsz = iommu_info_size;

	if (ioctl(vfio->fd, VFIO_IOMMU_GET_INFO, iommu_info)) {
		log_debug("failed to get iommu info\n");
		return NULL;
	}

	if (iommu_info->argsz > iommu_info_size) {
		log_info("iommu has extended info\n");

		iommu_info_size = iommu_info->argsz;
		iommu_info = realloc(iommu_info, iommu_info_size);
		memset(iommu_info, 0x0, iommu_info_size);
		iommu_info->argsz = iommu_info_size;

		if (ioctl(vfio->fd, VFIO_IOMMU_GET_INFO, iommu_info)) {
			log_debug("failed to get extended iommu info\n");
			return NULL;
		}
	}
	return iommu_info;
}

static int vfio_iommu_type1_get_capabilities(struct vfio_container *vfio)
{
	__autofree struct vfio_iommu_type1_info *iommu_info = NULL;
	struct vfio_info_cap_header *cap;

	iommu_info = vfio_iommu_type1_get_iommu_info(vfio);
	if (!iommu_info)
		return -1;

	if (!(iommu_info->flags & VFIO_IOMMU_INFO_CAPS))
		return 0;

	cap = (void *)iommu_info + iommu_info->cap_offset;

	do {
		switch (cap->id) {
# ifdef VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE
		case VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE:
			vfio_iommu_type1_get_cap_iova_ranges(&vfio->ctx, cap);
			break;
# endif
# ifdef VFIO_IOMMU_TYPE1_INFO_CAP_DMA_AVAIL
		case VFIO_IOMMU_TYPE1_INFO_CAP_DMA_AVAIL:
			vfio_iommu_type1_get_cap_dma_avail(cap);
			break;
# endif
		default:
			break;
		}

		if (!cap->next)
			break;

		cap = (void *)iommu_info + cap->next;
	} while (true);

	return 0;
}
#endif /* VFIO_IOMMU_INFO_CAPS */

static bool __iova_reserve(struct iommu_iova_range *ranges, int nranges, uint64_t *next,
			   size_t len, uint64_t *iova)
{
	uint64_t _next = *next;

	for (int i = 0; i < nranges; i++) {
		struct iommu_iova_range *r = &ranges[i];

		if (r->last < _next)
			continue;

		_next = max_t(uint64_t, _next, r->start);

		if (_next > r->last || r->last - _next + 1 < len)
			continue;

		*iova = _next;
		*next = _next + len;

		return true;
	}

	return false;
}

static int vfio_iommu_type1_iova_reserve(struct iommu_ctx *ctx, size_t len, uint64_t *iova,
					 unsigned long flags)
{
	struct vfio_container *vfio = container_of_var(ctx, vfio, ctx);

	__autolock(&vfio->lock);

	if (!ALIGNED(len, __VFN_PAGESIZE)) {
		log_debug("len is not page aligned\n");
		errno = EINVAL;
		return -1;
	}

	if (flags & IOMMU_MAP_EPHEMERAL) {
		if (!__iova_reserve(&vfio->ephemerals, 1, &vfio->next_ephemeral, len, iova))
			goto enomem;

		atomic_inc(&vfio->nephemerals);

		return 0;
	}

	if (__iova_reserve(ctx->iova_ranges, ctx->nranges, &vfio->next, len, iova))
		return 0;

enomem:
	errno = ENOMEM;
	return -1;
}

static int vfio_iommu_type1_init(struct vfio_container *vfio)
{
	uint64_t iova;

	if (vfio->iommu_set)
		return 0;

	if (ioctl(vfio->fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)) {
		log_debug("failed to set vfio iommu type\n");
		return -1;
	}

	vfio->iommu_set = true;

#ifdef VFIO_IOMMU_INFO_CAPS
	if (vfio_iommu_type1_get_capabilities(vfio)) {
		log_debug("failed to get iommu capabilities\n");
		return -1;
	}
#endif

	if (vfio_iommu_type1_iova_reserve(&vfio->ctx, VFIO_IOMMU_TYPE1_IOVA_RESERVED, &iova, 0x0)) {
		log_debug("could not reserve iova range\n");
		return -1;
	}

	vfio->ephemerals.start = iova;
	vfio->ephemerals.last = iova + VFIO_IOMMU_TYPE1_IOVA_RESERVED - 1;

	if (logv(LOG_INFO)) {
		__autofree char *str;

		log_fatal_if(iommu_iova_range_to_string(&vfio->ephemerals, &str) < 0,
			     "iommu_iova_range_to_string\n");

		log_info("reserved 64k for ephemerals %s\n", str);
	}

	return 0;
}

static int vfio_group_set_container(struct vfio_group *group, struct vfio_container *vfio)
{
	log_info("adding group '%s' to container\n", group->path);

	if (ioctl(group->fd, VFIO_GROUP_SET_CONTAINER, &vfio->fd)) {
		log_debug("failed to add group to vfio container\n");
		return -1;
	}

	if (vfio_iommu_type1_init(vfio)) {
		log_debug("failed to configure iommu\n");

		log_fatal_if(ioctl(group->fd, VFIO_GROUP_UNSET_CONTAINER), "unset container\n");

		return -1;
	}

	return 0;
}

static int vfio_group_open(const char *path)
{
	int fd;

	struct vfio_group_status group_status = {
		.argsz = sizeof(group_status),
	};

	fd = open(path, O_RDWR);
	if (fd < 0) {
		log_debug("failed to open vfio group file: %s\n", strerror(errno));
		return -1;
	}

	if (ioctl(fd, VFIO_GROUP_GET_STATUS, &group_status)) {
		log_debug("failed to get vfio group status\n");
		goto close_fd;
	}

	if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		errno = EINVAL;
		log_debug("vfio group is not viable\n");
		goto close_fd;
	}

	return fd;

close_fd:
	log_fatal_if(close(fd), "close\n");

	return -1;
}

static int vfio_get_group_fd(struct vfio_container *vfio, const char *path)
{
	struct vfio_group *group;
	int i;

	for (i = 0; i < VFN_MAX_VFIO_GROUPS; i++) {
		group = &vfio->groups[i];

		if (group->path && streq(group->path, path))
			return group->fd;
	}

	for (i = 0; i < VFN_MAX_VFIO_GROUPS; i++) {
		group = &vfio->groups[i];

		if (!group->path)
			break;
	}

	if (i == VFN_MAX_VFIO_GROUPS) {
		errno = EMFILE;
		return -1;
	}

	group->path = strdup(path);
	if (!group->path)
		return -1;

	group->fd = vfio_group_open(group->path);
	if (group->fd < 0) {
		log_debug("failed to open vfio group\n");
		goto free_group_path;
	}

	if (vfio_group_set_container(group, vfio)) {
		log_fatal_if(close(group->fd), "close group fd\n");
		goto free_group_path;
	}

	return group->fd;

free_group_path:
	free(group->path);

	return -1;
}

static int vfio_get_device_fd(struct iommu_ctx *ctx, const char *bdf)
{
	struct vfio_container *vfio = container_of_var(ctx, vfio, ctx);

	__autofree char *group = NULL;
	int gfd, ret_fd;

	group = pci_get_iommu_group(bdf);
	if (!group) {
		log_debug("could not determine iommu group for device %s\n", bdf);
		errno = EINVAL;
		return -1;
	}
	log_info("vfio iommu group is %s\n", group);

	gfd = vfio_get_group_fd(vfio, group);
	if (gfd < 0)
		return -1;

	ret_fd = ioctl(gfd, VFIO_GROUP_GET_DEVICE_FD, bdf);
	if (ret_fd < 0) {
		log_debug("failed to get device fd\n");
		return -1;
	}

	return ret_fd;
}

static int vfio_iommu_type1_do_dma_map(struct iommu_ctx *ctx, struct iova_mapping *m)
{
	struct vfio_container *vfio = container_of_var(ctx, vfio, ctx);

	struct vfio_iommu_type1_dma_map dma_map = {
		.argsz = sizeof(dma_map),
		.vaddr = (uintptr_t)m->vaddr,
		.size  = m->len,
		.iova  = m->iova,
		.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
	};

	if (m->flags & IOMMU_MAP_NOWRITE)
		dma_map.flags &= ~VFIO_DMA_MAP_FLAG_WRITE;

	if (m->flags & IOMMU_MAP_NOREAD)
		dma_map.flags &= ~VFIO_DMA_MAP_FLAG_READ;

	trace_guard(VFIO_IOMMU_TYPE1_MAP_DMA) {
		trace_emit("vaddr %p iova 0x%" PRIx64 " len %zu\n", m->vaddr, m->iova, m->len);
	}

	if (!ALIGNED(((uintptr_t)m->vaddr | m->len | m->iova), __VFN_PAGESIZE)) {
		log_debug("vaddr, len or iova not page aligned\n");
		errno = EINVAL;
		return -1;
	}

	if (ioctl(vfio->fd, VFIO_IOMMU_MAP_DMA, &dma_map)) {
		log_debug("could not map\n");
		return -1;
	}

	return 0;
}

static int vfio_iommu_type1_do_dma_unmap(struct iommu_ctx *ctx, struct iova_mapping *m)
{
	struct vfio_container *vfio = container_of_var(ctx, vfio, ctx);
	uint64_t iova = m->iova;
	size_t len = m->len;

	struct vfio_iommu_type1_dma_unmap dma_unmap = {
		.argsz = sizeof(dma_unmap),
		.size = len,
		.iova = iova,
	};

	trace_guard(VFIO_IOMMU_TYPE1_UNMAP_DMA) {
		trace_emit("iova 0x%" PRIx64 " len %zu\n", iova, len);
	}

	if (ioctl(vfio->fd, VFIO_IOMMU_UNMAP_DMA, &dma_unmap)) {
		log_debug("could not unmap\n");
		return -1;
	}

	return 0;
}

static void vfio_iommu_type1_recycle_ephemeral_iovas(struct vfio_container *vfio)
{
	__autolock(&vfio->lock);

	trace_guard(VFIO_IOMMU_TYPE1_RECYCLE_EPHEMERAL_IOVAS) {
		trace_emit("recycling ephemeral range (0x%" PRIx64 " -> 0x%llx)\n",
			   vfio->next_ephemeral, vfio->ephemerals.start);
	}

	vfio->next_ephemeral = vfio->ephemerals.start;
}

static void vfio_iommu_type1_iova_put_ephemeral(struct iommu_ctx *ctx)
{
	struct vfio_container *vfio = container_of_var(ctx, vfio, ctx);

	if (atomic_dec_fetch(&vfio->nephemerals) == 0)
		vfio_iommu_type1_recycle_ephemeral_iovas(vfio);
}

#ifdef VFIO_UNMAP_ALL
static int vfio_iommu_type1_do_dma_unmap_all(struct iommu_ctx *ctx)
{
	struct vfio_container *vfio = container_of_var(ctx, vfio, ctx);

	struct vfio_iommu_type1_dma_unmap dma_unmap = {
		.argsz = sizeof(dma_unmap),
		.flags = VFIO_DMA_UNMAP_FLAG_ALL,
	};

	if (ioctl(vfio->fd, VFIO_IOMMU_UNMAP_DMA, &dma_unmap)) {
		log_debug("failed to unmap dma\n");
		return -1;
	}

	return 0;
}
#endif

static const struct iommu_ctx_ops vfio_ops = {
	.get_device_fd = vfio_get_device_fd,

	.iova_reserve = vfio_iommu_type1_iova_reserve,
	.iova_put_ephemeral = vfio_iommu_type1_iova_put_ephemeral,

	.dma_map = vfio_iommu_type1_do_dma_map,
	.dma_unmap = vfio_iommu_type1_do_dma_unmap,
#ifdef VFIO_UNMAP_ALL
	.dma_unmap_all = vfio_iommu_type1_do_dma_unmap_all,
#endif
};

static int vfio_init_container(struct vfio_container *vfio)
{
	vfio->fd = open("/dev/vfio/vfio", O_RDWR);
	if (vfio->fd < 0) {
		log_debug("failed to open vfio device\n");
		return -1;
	}

	if (ioctl(vfio->fd, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
		log_debug("invalid vfio version\n");
		return -1;
	}

	if (!ioctl(vfio->fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
		log_debug("vfio type 1 iommu not supported\n");
		return -1;
	}

	memcpy(&vfio->ctx.ops, &vfio_ops, sizeof(vfio->ctx.ops));

	return 0;
}

struct iommu_ctx *vfio_get_iommu_context(const char *name)
{
	struct vfio_container *vfio = znew_t(struct vfio_container, 1);

	iommu_ctx_init(&vfio->ctx);

	if (vfio_init_container(vfio) < 0) {
		free(vfio);
		return NULL;
	}

	vfio->name = strdup(name);

	return &vfio->ctx;
}

struct iommu_ctx *vfio_get_default_iommu_context(void)
{
	if (vfio_default_container.fd == -1) {
		iommu_ctx_init(&vfio_default_container.ctx);

		log_fatal_if(vfio_init_container(&vfio_default_container),
			     "init default container\n");
	}

	return &vfio_default_container.ctx;
}
