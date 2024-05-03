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


#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>

extern size_t __VFN_PAGESIZE;
extern int __VFN_PAGESHIFT;

void backtrace_abort(void);

static inline void __do_autofree(void *mem)
{
	free(mem);
}

#define __autofree __attribute__((cleanup(__do_autofree)))

static inline bool would_overflow(unsigned int n, size_t sz)
{
	return n > SIZE_MAX / sz;
}

static inline void *zmalloc(size_t sz)
{
	void *mem;

	if (unlikely(!sz))
		return NULL;

	sz += sizeof(uint64_t);
	mem = IOMallocZero(sz);
	if (unlikely(!mem)) {
		log_debug("IOMallocZero failed!");
		backtrace_abort();
	}

	*(uint64_t *)mem = sz; // Save length of malloc -- used on free.
	return (void *)(((uint8_t *)mem) + sizeof(uint64_t));
}

void *mallocn(unsigned int n, size_t sz);

static inline void *zmallocn(unsigned int n, size_t sz)
{
	return zmalloc(n * sz);
}

inline void free(void *ptr)
{
	void *base_ptr = (void *)(((uint8_t *)ptr)-sizeof(uint64_t));
	uint64_t length = *(uint64_t *)base_ptr;

	IOFree(base_ptr, length);
}
