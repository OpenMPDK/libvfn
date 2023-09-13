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

#define log_fmt(fmt) "iommu/context: " fmt

#include "vfn/iommu.h"
#include "vfn/support.h"

#include "context.h"

#define IOVA_MIN 0x10000
#define IOVA_MAX_39BITS (1ULL << 39)

#ifdef HAVE_VFIO_DEVICE_BIND_IOMMUFD
static bool __iommufd_broken;

static void __attribute__((constructor)) __check_iommufd_broken(void)
{
	struct stat sb;

	if (stat("/dev/vfio/devices", &sb) || !S_ISDIR(sb.st_mode)) {
		log_info("iommufd broken; probably missing CONFIG_VFIO_DEVICE_CDEV=y\n");

		__iommufd_broken = true;
	}
}
#endif

struct iommu_ctx *iommu_get_default_context(void)
{
#ifdef HAVE_VFIO_DEVICE_BIND_IOMMUFD
	if (__iommufd_broken)
		goto fallback;

	return iommufd_get_default_iommu_context();

fallback:
#endif
#ifdef __APPLE__
	return driverkit_get_default_iommu_context();
#else
	return vfio_get_default_iommu_context();
#endif
}

struct iommu_ctx *iommu_get_context(const char *name)
{
#ifndef __APPLE__
#ifdef HAVE_VFIO_DEVICE_BIND_IOMMUFD
	if (__iommufd_broken)
		goto fallback;

	return iommufd_get_iommu_context(name);

fallback:
#endif
	return vfio_get_iommu_context(name);
#else
	return driverkit_get_iommu_context(name);
#endif
}

#ifndef __APPLE__
void iommu_ctx_init(struct iommu_ctx *ctx)
{
	ctx->nranges = 1;
	ctx->iova_ranges = znew_t(struct iommu_iova_range, ctx->nranges);

	/*
	 * For vfio, if we end up not being able to get a list of allowed
	 * iova ranges, be conservative.
	 */
	ctx->iova_ranges[0].start = IOVA_MIN;
	ctx->iova_ranges[0].last = IOVA_MAX_39BITS - 1;

	pthread_mutex_init(&ctx->lock, NULL);

	skiplist_init(&ctx->map.list);
	pthread_mutex_init(&ctx->map.lock, NULL);
}
#endif
