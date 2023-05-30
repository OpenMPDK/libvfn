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

#include <assert.h>
#include <byteswap.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <linux/limits.h>
#include <linux/pci_regs.h>
#include <linux/vfio.h>

#include <vfn/support/atomic.h>
#include <vfn/support/compiler.h>
#include <vfn/support/log.h>
#include <vfn/support/mem.h>
#include <vfn/vfio/iommu.h>
#include <vfn/vfio/state.h>

#include "iommu.h"

int vfio_set_irq(struct vfio_state *vfio, int *eventfds, unsigned int count)
{
	struct vfio_irq_set *irq_set;
	uint32_t irq_set_size;
	int ret;

	if (!(vfio->irq_info.flags & VFIO_IRQ_INFO_EVENTFD)) {
		errno = EINVAL;
		log_debug("device irq does not support eventfd\n");
		return -1;
	}

	irq_set_size = sizeof(*irq_set) + sizeof(int) * count;
	irq_set = xmalloc(irq_set_size);

	*irq_set = (struct vfio_irq_set) {
		.argsz = irq_set_size,
		.flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
		.index = vfio->irq_info.index,
		.start = 0,
		.count = count,
	};

	memcpy(irq_set->data, eventfds, sizeof(int) * count);

	ret = ioctl(vfio->device, VFIO_DEVICE_SET_IRQS, irq_set);
	free(irq_set);

	if (ret) {
		log_debug("failed to set device irq\n");
		return -1;
	}

	return 0;
}

int vfio_disable_irq(struct vfio_state *vfio)
{
	struct vfio_irq_set irq_set;
	int ret;

	irq_set = (struct vfio_irq_set) {
		.argsz = sizeof(irq_set),
		.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
		.index = vfio->irq_info.index,
	};

	ret = ioctl(vfio->device, VFIO_DEVICE_SET_IRQS, &irq_set);

	if (ret) {
		log_debug("failed to disable device irq\n");
		return -1;
	}

	return 0;
}

int vfio_reset(struct vfio_state *vfio)
{
	if (!(vfio->device_info.flags & VFIO_DEVICE_FLAGS_RESET)) {
		errno = ENOTSUP;
		return -1;
	}

	return ioctl(vfio->device, VFIO_DEVICE_RESET);
}

#ifdef VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE
static void iommu_get_iova_ranges(struct vfio_iommu_state *iommu, struct vfio_info_cap_header *cap)
{
	struct vfio_iommu_type1_info_cap_iova_range *cap_iova_range;
	size_t len;

	cap_iova_range = (struct vfio_iommu_type1_info_cap_iova_range *)cap;
	len = sizeof(struct vfio_iova_range) * cap_iova_range->nr_iovas;

	iommu->nranges = cap_iova_range->nr_iovas;
	iommu->iova_ranges = realloc(iommu->iova_ranges, len);
	memcpy(iommu->iova_ranges, cap_iova_range->iova_ranges, len);

	if (logv(LOG_INFO)) {
		for (unsigned int i = 0; i < iommu->nranges; i++) {
			struct vfio_iova_range *r = &iommu->iova_ranges[i];

			log_info("iova range %d is [0x%llx; 0x%llx]\n", i, r->start, r->end);
		}
	}
}
#endif

#ifdef VFIO_IOMMU_INFO_CAPS
static void vfio_iommu_init_capabilities(struct vfio_iommu_state *iommu,
					 struct vfio_iommu_type1_info *iommu_info)
{
	struct vfio_info_cap_header *cap = (void *)iommu_info + iommu_info->cap_offset;

	do {
		switch (cap->id) {
#ifdef VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE
		case VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE:
			iommu_get_iova_ranges(iommu, cap);
			break;
		}
#endif

		if (!cap->next)
			break;

		cap = (void *)iommu_info + cap->next;
	} while (true);
}
#endif

int vfio_open(struct vfio_state *vfio, const char *group)
{
	struct vfio_group_status group_status = {
		.argsz = sizeof(group_status),
	};

	__autofree struct vfio_iommu_type1_info *iommu_info = NULL;
	uint32_t iommu_info_size = sizeof(*iommu_info);

	memset(vfio, 0x0, sizeof(*vfio));

	vfio->container = open("/dev/vfio/vfio", O_RDWR);
	if (vfio->container < 0) {
		log_debug("failed to open vfio device\n");
		return -1;
	}

	if (ioctl(vfio->container, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
		log_debug("invalid vfio version\n");
		goto err_close_container;
	}

	if (!ioctl(vfio->container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
		log_debug("vfio type 1 iommu not supported\n");
		goto err_close_container;
	}

	vfio->group = open(group, O_RDWR);
	if (vfio->group < 0) {
		log_debug("failed to open vfio group file: %s\n", strerror(errno));
		goto err_close_container;
	}

	if (ioctl(vfio->group, VFIO_GROUP_GET_STATUS, &group_status)) {
		log_debug("failed to get vfio group status\n");
		goto err;
	}

	if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		errno = EINVAL;
		log_debug("vfio group is not viable\n");
		goto err;
	}

	if (ioctl(vfio->group, VFIO_GROUP_SET_CONTAINER, &vfio->container)) {
		log_debug("failed to add group to vfio container\n");
		goto err;
	}

	if (ioctl(vfio->container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)) {
		log_debug("failed to set vfio iommu type\n");
		goto err;
	}

	vfio_iommu_init(&vfio->iommu);

	iommu_info = xmalloc(iommu_info_size);
	memset(iommu_info, 0x0, iommu_info_size);
	iommu_info->argsz = iommu_info_size;

	if (ioctl(vfio->container, VFIO_IOMMU_GET_INFO, iommu_info)) {
		log_debug("failed to get iommu info\n");
		goto err;
	}

	if (iommu_info->argsz > iommu_info_size) {
		log_info("iommu has extended info\n");

		iommu_info_size = iommu_info->argsz;
		iommu_info = realloc(iommu_info, iommu_info_size);
		memset(iommu_info, 0x0, iommu_info_size);
		iommu_info->argsz = iommu_info_size;

		if (ioctl(vfio->container, VFIO_IOMMU_GET_INFO, iommu_info)) {
			log_debug("failed to get extended iommu info\n");
			vfio_iommu_close(&vfio->iommu);
			goto err;
		}

#ifdef VFIO_IOMMU_INFO_CAPS
		if (iommu_info->flags & VFIO_IOMMU_INFO_CAPS)
			vfio_iommu_init_capabilities(&vfio->iommu, iommu_info);
#endif
	}

	return 0;

err:
	close(vfio->group);
err_close_container:
	close(vfio->container);

	return -1;
}

void vfio_close(struct vfio_state *vfio)
{
	vfio_iommu_close(&vfio->iommu);

	if (vfio->device_info.flags & VFIO_DEVICE_FLAGS_RESET) {
		if (vfio_reset(vfio))
			log_debug("could not reset: %s\n", strerror(errno));
	}

	close(vfio->device);
	close(vfio->group);
	close(vfio->container);
}

int vfio_map_vaddr(struct vfio_state *vfio, void *vaddr, size_t len, uint64_t *iova)
{
	uint64_t _iova;

	if (vfio_iommu_vaddr_to_iova(&vfio->iommu, vaddr, &_iova))
		goto out;

	if (vfio_iommu_allocate_sticky_iova(&vfio->iommu, len, &_iova)) {
		log_debug("failed to allocate iova\n");
		return -1;
	}

	if (vfio_iommu_map_dma(&vfio->iommu, vaddr, len, _iova)) {
		log_debug("failed to map dma\n");
		return -1;
	}

	if (vfio_iommu_add_mapping(&vfio->iommu, vaddr, len, _iova)) {
		log_debug("failed to add mapping\n");
		return -1;
	}

out:
	if (iova)
		*iova = _iova;

	return 0;
}

int vfio_unmap_vaddr(struct vfio_state *vfio, void *vaddr)
{
	struct vfio_iommu_mapping *m;

	m = vfio_iommu_find_mapping(&vfio->iommu, vaddr);
	if (!m)
		return 0;

	if (vfio_iommu_unmap_dma(&vfio->iommu, m->len, m->iova)) {
		log_debug("failed to unmap dma\n");
		return -1;
	}

	vfio_iommu_remove_mapping(&vfio->iommu, m->vaddr);

	return 0;
}

int vfio_map_vaddr_ephemeral(struct vfio_state *vfio, void *vaddr, size_t len, uint64_t *iova)
{
	if (vfio_iommu_allocate_ephemeral_iova(&vfio->iommu, len, iova)) {
		log_error("failed to allocate ephemeral iova\n");

		return -1;
	}

	if (vfio_iommu_map_dma(&vfio->iommu, vaddr, len, *iova)) {
		log_error("failed to map dma\n");

		if (atomic_dec_fetch(&vfio->iommu.nephemeral) == 0)
			vfio_iommu_recycle_ephemeral_iovas(&vfio->iommu);

		return -1;
	}

	return 0;
}

int vfio_unmap_ephemeral_iova(struct vfio_state *vfio, size_t len, uint64_t iova)
{
	return vfio_iommu_unmap_ephemeral_iova(&vfio->iommu, len, iova);
}
