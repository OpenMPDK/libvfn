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

#ifndef LIBVFN_SUPPORT_H
#define LIBVFN_SUPPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __APPLE__
#include <byteswap.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <dirent.h>
#include <assert.h>
#include <string.h>

#include <sys/mman.h>

#include <linux/vfio.h>
#else
#include <vfn/support/platform/macos/byteswap.h>
#include <vfn/support/platform/macos/errno.h>
#endif

#include <inttypes.h> // PRIx64 used for logging
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include <vfn/support/align.h>
#include <vfn/support/atomic.h>
#include <vfn/support/autoptr.h>
#include <vfn/support/barrier.h>
#include <vfn/support/compiler.h>
#include <vfn/support/endian.h>
#include <vfn/support/log.h>
#include <vfn/support/mem.h>
#include <vfn/support/mmio.h>
#include <vfn/support/mutex.h>
#include <vfn/support/ticks.h>
#ifndef __APPLE__
#include <vfn/support/io.h>
#include <vfn/support/timer.h>
#endif

#ifdef __cplusplus
}
#endif

#endif /* LIBVFN_SUPPORT_H */
