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

#ifndef LIBVFN_SUPPORT_MEM_H
#define LIBVFN_SUPPORT_MEM_H

ssize_t __pgmap(void **mem, size_t sz, void **opaque);
ssize_t __pgmapn(void **mem, unsigned int n, size_t sz, void **opaque);
void __pgunmap(void *mem, size_t len, void *opaque);

#define _new_t(t, n, f) \
	((t *) f(n, sizeof(t)))

#define new_t(t, n) _new_t(t, n, mallocn)
#define znew_t(t, n) _new_t(t, n, zmallocn)

ssize_t pgmap(void **mem, size_t sz);
ssize_t pgmapn(void **mem, unsigned int n, size_t sz);

void pgunmap(void *mem, size_t len);

#ifdef __APPLE__
#include <vfn/support/platform/macos/mem.h>
#else
#include <vfn/support/platform/linux/mem.h>
#endif

#endif /* LIBVFN_SUPPORT_MEM_H */
