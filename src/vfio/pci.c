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

#define log_fmt(fmt) "vfio/pci: " fmt

#include <linux/limits.h>
#include <linux/vfio.h>
#include <linux/pci_regs.h>
#include <sys/ioctl.h>

#include "vfn/support.h"
#include "vfn/iommu.h"
#include "vfn/vfio.h"
#include "vfn/pci.h"

#include "ccan/array_size/array_size.h"
#include "ccan/minmax/minmax.h"
#include "ccan/str/str.h"
#include "ccan/list/list.h"

#include "iommu/context.h"

static int pci_set_bus_master(struct vfio_pci_device *pci)
{
	uint16_t pci_cmd;

	if (vfio_pci_read_config(pci, &pci_cmd, sizeof(pci_cmd), PCI_COMMAND) < 0) {
		log_error("failed to read pci config region\n");
		return -1;
	}

	pci_cmd |= PCI_COMMAND_MASTER;

	if (vfio_pci_write_config(pci, &pci_cmd, sizeof(pci_cmd), PCI_COMMAND) < 0) {
		log_error("failed to write pci config region\n");
		return -1;
	}

	return 0;
}

static int vfio_pci_init_bar(struct vfio_pci_device *pci, int idx)
{
	assert(idx < PCI_STD_NUM_BARS);

	pci->bar_region_info[idx] = (struct vfio_region_info) {
		.index = VFIO_PCI_BAR0_REGION_INDEX + idx,
		.argsz = sizeof(struct vfio_region_info),
	};

	if (ioctl(pci->dev.fd, VFIO_DEVICE_GET_REGION_INFO, &pci->bar_region_info[idx])) {
		log_error("failed to get bar region info\n");
		return -1;
	}

	return 0;
}

static int vfio_pci_init_irq(struct vfio_pci_device *pci)
{
	int irq_index = VFIO_PCI_MSIX_IRQ_INDEX;

	memset(&pci->dev.irq_info, 0x0, sizeof(pci->dev.irq_info));
	pci->dev.irq_info.argsz = sizeof(pci->dev.irq_info);

	/* prefer msi-x/msi and fall back to intx */
	do {
		if (irq_index < 0) {
			errno = EINVAL;
			log_error("no supported irq types\n");
			return -1;
		}

		pci->dev.irq_info.index = irq_index--;

		if (ioctl(pci->dev.fd, VFIO_DEVICE_GET_IRQ_INFO, &pci->dev.irq_info)) {
			log_error("failed to get device irq info\n");
			return -1;
		}
	} while (!pci->dev.irq_info.count);

	log_info("irq_info.count %d\n", pci->dev.irq_info.count);

	return 0;
}

void *vfio_pci_map_bar(struct vfio_pci_device *pci, int idx, size_t len, uint64_t offset,
		       int prot)
{
	void *mem;

	assert(idx < PCI_STD_NUM_BARS);

	len = min_t(size_t, len, pci->bar_region_info[idx].size - offset);
	offset = pci->bar_region_info[idx].offset + offset;

	mem = mmap(NULL, len, prot, MAP_SHARED, pci->dev.fd, offset);
	if (mem == MAP_FAILED) {
		log_debug("failed to map bar region\n");
		mem = NULL;
	}

	return mem;
}

void vfio_pci_unmap_bar(struct vfio_pci_device *pci, int idx, void *mem, size_t len,
			uint64_t offset)
{
	assert(idx < PCI_STD_NUM_BARS);

	len = min_t(size_t, len, pci->bar_region_info[idx].size - offset);

	if (munmap(mem, len))
		log_debug("failed to unmap bar region\n");
}

int vfio_pci_open(struct vfio_pci_device *pci, const char *bdf)
{
	pci->bdf = bdf;

	if (!pci->dev.ctx)
		pci->dev.ctx = iommu_get_default_context();

	pci->dev.fd = pci->dev.ctx->ops.get_device_fd(pci->dev.ctx, bdf);
	if (pci->dev.fd < 0) {
		log_error("failed to get device fd\n");
		return -1;
	}

	pci->dev.device_info.argsz = sizeof(struct vfio_device_info);

	if (ioctl(pci->dev.fd, VFIO_DEVICE_GET_INFO, &pci->dev.device_info)) {
		log_error("failed to get device info\n");
		return -1;
	}

	assert(pci->dev.device_info.flags & VFIO_DEVICE_FLAGS_PCI);

	pci->config_region_info = (struct vfio_region_info) {
		.argsz = sizeof(pci->config_region_info),
		.index = VFIO_PCI_CONFIG_REGION_INDEX,
	};

	if (ioctl(pci->dev.fd, VFIO_DEVICE_GET_REGION_INFO, &pci->config_region_info)) {
		log_error("failed to get config region info\n");
		return -1;
	}

	for (int i = 0; i < PCI_STD_NUM_BARS; i++) {
		if (vfio_pci_init_bar(pci, i))
			return -1;
	}

	if (pci_set_bus_master(pci)) {
		log_error("failed to set pci bus master\n");
		return -1;
	}

	if (vfio_pci_init_irq(pci)) {
		log_error("failed to initialize irq\n");
		return -1;
	}

	return 0;
}
