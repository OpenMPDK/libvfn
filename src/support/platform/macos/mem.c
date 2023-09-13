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

#ifdef __cplusplus
extern "C" {
#endif

#include <vfn/support.h>

size_t __VFN_PAGESIZE;
int __VFN_PAGESHIFT;

static void __attribute__((constructor)) init_page_size(void)
{
	__VFN_PAGESIZE = IOVMPageSize;
	__VFN_PAGESHIFT = 63u - __builtin_clzl(__VFN_PAGESIZE);

	log_debug("pagesize is %zu (shift %d)\n", __VFN_PAGESIZE, __VFN_PAGESHIFT);
}

void backtrace_abort(void)
{
	abort();
}

ssize_t __pgmap(void **mem, size_t sz, void **opaque)
{
	IOAddressSegment virtualAddressSegment;
	ssize_t len = ALIGN_UP(sz, __VFN_PAGESIZE);

	IOBufferMemoryDescriptor **mem_descriptor = (IOBufferMemoryDescriptor **) opaque;
	IOBufferMemoryDescriptor::Create(
		kIOMemoryDirectionInOut,
		len,
		__VFN_PAGESIZE,
		mem_descriptor
	);
	(*mem_descriptor)->SetLength(len);
	(*mem_descriptor)->GetAddressRange(&virtualAddressSegment);
	bzero((void *) virtualAddressSegment.address, virtualAddressSegment.length);
	*mem = (void *) virtualAddressSegment.address;

	return len;
}

ssize_t __pgmapn(void **mem, unsigned int n, size_t sz, void **opaque)
{
	return __pgmap(mem, n * sz, opaque);
}

void __pgunmap(void *mem, size_t len, void *opaque)
{
	IOBufferMemoryDescriptor *_mem = (IOBufferMemoryDescriptor *) opaque;

	OSSafeReleaseNULL(_mem);
}

ssize_t pgmap(void **mem, size_t sz)
{
	log_fatal("Use ::__pgmap on macOS");
}

ssize_t pgmapn(void **mem, unsigned int n, size_t sz)
{
	log_fatal("Use ::__pgmapn on macOS");
}

void pgunmap(void *mem, size_t len)
{
	log_fatal("Use ::__pgunmap on macOS");
}

#ifdef __cplusplus
}
#endif
