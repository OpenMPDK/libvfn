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

#ifndef LIBVFN_NVME_H
#define LIBVFN_NVME_H

#include <vfn/support.h>

#ifndef __APPLE__
#include <vfn/vfio.h>
#else
#define NVME_AQ_QSIZE 32
#include <vfn/driverkit.h>
#endif

#include <vfn/trace.h>
#include <vfn/iommu.h>
#include <vfn/nvme/types.h>
#include <vfn/nvme/queue.h>
#include <vfn/nvme/ctrl.h>
#include <vfn/nvme/util.h>
#include <vfn/nvme/rq.h>

#endif /* LIBVFN_NVME_H */
