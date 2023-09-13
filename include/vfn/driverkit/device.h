/* SPDX-License-Identifier: LGPL-2.1-or-later or MIT */

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2022 The libvfn Authors. All Rights Reserved.
 *
 * This library (libvfn) is dual licensed under the GNU Lesser General
 * Public License version 2.1 or later or the MIT license. See the
 * COPYING and LICENSE files for more information.
 */

#ifndef LIBVFN_DRIVERKIT_DEVICE_H
#define LIBVFN_DRIVERKIT_DEVICE_H

#include <DriverKit/OSCollection.h>
#include <PCIDriverKit/PCIDriverKit.h>
#include "iommu/context.h"

struct driverkit_bar_info {
	uint8_t type;
	uint8_t memory_index;
	uint64_t size;
};

struct driverkit_pci_device {
	IOPCIDevice *dev;
	struct iommu_ctx ctx;
	OSDictionary *iommu_mappings;
	const char *bdf;

	struct driverkit_bar_info bar_region_info[6];
};

#define __iommu_ctx(x) (&((struct driverkit_pci_device *)(x))->ctx)

#endif /* LIBVFN_DRIVERKIT_DEVICE_H */
