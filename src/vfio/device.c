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

#define log_fmt(fmt) "vfio/device: " fmt

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/limits.h>
#include <linux/pci_regs.h>

#include "vfn/support.h"
#include "vfn/vfio.h"

#include "ccan/array_size/array_size.h"
#include "ccan/minmax/minmax.h"
#include "ccan/str/str.h"

int vfio_set_irq(struct vfio_device *dev, int *eventfds, int count)
{
	struct vfio_irq_set *irq_set;
	size_t irq_set_size;
	int ret;

	if (!(dev->irq_info.flags & VFIO_IRQ_INFO_EVENTFD)) {
		errno = EINVAL;
		log_error("device irq does not support eventfd\n");
		return -1;
	}

	irq_set_size = sizeof(*irq_set) + sizeof(int) * count;
	irq_set = xmalloc(irq_set_size);

	*irq_set = (struct vfio_irq_set) {
		.argsz = (uint32_t)irq_set_size,
		.flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
		.index = dev->irq_info.index,
		.start = 0,
		.count = count,
	};

	memcpy(irq_set->data, eventfds, sizeof(int) * count);

	ret = ioctl(dev->fd, VFIO_DEVICE_SET_IRQS, irq_set);
	free(irq_set);

	if (ret) {
		log_error("failed to set device irq\n");
		return -1;
	}

	return 0;
}

int vfio_disable_irq(struct vfio_device *dev)
{
	struct vfio_irq_set irq_set;
	int ret;

	irq_set = (struct vfio_irq_set) {
		.argsz = sizeof(irq_set),
		.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
		.index = dev->irq_info.index,
	};

	ret = ioctl(dev->fd, VFIO_DEVICE_SET_IRQS, &irq_set);

	if (ret) {
		log_error("failed to disable device irq\n");
		return -1;
	}

	return 0;
}

int vfio_reset(struct vfio_device *dev)
{
	if (!(dev->device_info.flags & VFIO_DEVICE_FLAGS_RESET)) {
		errno = ENOTSUP;
		return -1;
	}

	return ioctl(dev->fd, VFIO_DEVICE_RESET);
}
