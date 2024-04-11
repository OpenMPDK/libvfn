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

#include <vfn/nvme.h>
#include "iommu/context.h"
#ifndef __APPLE__
#include "ccan/time/time.h"
#endif

void nvme_cq_get_cqes(struct nvme_cq *cq, struct nvme_cqe *cqes, int n)
{
	struct nvme_cqe *cqe;

	do {
		cqe = nvme_cq_get_cqe(cq);
		if (!cqe)
			continue;

		n--;

		if (cqes)
			memcpy(cqes++, cqe, sizeof(*cqe));
	} while (n > 0);
}

int nvme_cq_wait_cqes(struct nvme_cq *cq, struct nvme_cqe *cqes, int n, uint64_t timeout_ns)
{
	struct nvme_cqe *cqe;
	uint64_t timeout;

	if (!timeout_ns) {
		nvme_cq_get_cqes(cq, cqes, n);

		return 0;
	}

	timeout = get_ticks() + (timeout_ns*1000) * (__vfn_ticks_freq / 1000000ULL);

	do {
		cqe = nvme_cq_get_cqe(cq);
		if (!cqe)
			continue;

		n--;

		if (cqes)
			memcpy(cqes++, cqe, sizeof(*cqe));
	} while (n > 0 && get_ticks() < timeout);

	if (n > 0)
		errno = ETIMEDOUT;

	return n;
}
