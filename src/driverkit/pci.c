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



#define log_fmt(fmt) "driverkit/pci: " fmt

#ifdef __cplusplus
extern "C" {
#endif

#include <vfn/support.h>

// NOTE: Faux open for macOS
int vfio_pci_open(struct driverkit_pci_device *pci, const char *bdf)
{
	return 0;
}

void *vfio_pci_map_bar(struct driverkit_pci_device *pci, int idx, size_t len, uint64_t offset,
	int prot)
{
	struct macvfn_pci_map_bar *mapping = (struct macvfn_pci_map_bar *) zmallocn(1,
		sizeof(struct macvfn_pci_map_bar));
	mapping->pci = pci;
	mapping->idx = idx;
	mapping->len = len;
	mapping->offset = offset;

	return mapping;
}

void vfio_pci_unmap_bar(struct driverkit_pci_device *pci, int idx, void *mem, size_t len,
	uint64_t offset)
{
	free(mem);
}

#ifdef __cplusplus
}
#endif
