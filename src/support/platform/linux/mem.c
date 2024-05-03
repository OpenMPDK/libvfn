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

#define log_fmt(fmt) "support/mem: " fmt
#include <sys/stat.h>
#ifdef __GLIBC__
#include <execinfo.h>
#endif
#include "ccan/compiler/compiler.h"
#include <vfn/support.h>

size_t __VFN_PAGESIZE;
int __VFN_PAGESHIFT;

static void __attribute__((constructor)) init_page_size(void)
{
	__VFN_PAGESIZE = sysconf(_SC_PAGESIZE);
	__VFN_PAGESHIFT = 63u - __builtin_clzl(__VFN_PAGESIZE);

	log_debug("pagesize is %zu (shift %d)\n", __VFN_PAGESIZE, __VFN_PAGESHIFT);
}

void backtrace_abort(void)
{
#ifdef __GLIBC__
	void *buf[10];
	char **symbols = NULL;
	int size;

	size = backtrace(buf, 10);
	symbols = backtrace_symbols(buf, size);

	if (symbols) {
		fprintf(stderr, "fatal error; dumping maximum %d stack frames\n", size);

		for (int i = 0; i < size; i++)
			fprintf(stderr, "[%d]: %s\n", i, symbols[i]);
	}

	free(symbols);
#endif /* __GLIBC__ */
	abort();
}

ssize_t pgmap(void **mem, size_t sz)
{
	ssize_t len = ALIGN_UP(sz, __VFN_PAGESIZE);

	*mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	if (*mem == MAP_FAILED)
		return -1;

	return len;
}

ssize_t pgmapn(void **mem, unsigned int n, size_t sz)
{
	if (would_overflow(n, sz)) {
		fprintf(stderr, "allocation of %d * %zu bytes would overflow\n", n, sz);

		backtrace_abort();
	}

	return pgmap(mem, n * sz);
}

ssize_t __pgmap(void **mem, size_t sz, void **opaque UNUSED)
{
	return pgmap(mem, sz);
}

ssize_t __pgmapn(void **mem, unsigned int n, size_t sz, void **opaque UNUSED)
{
	return pgmapn(mem, n, sz);
}

void __pgunmap(void *mem, size_t len, void *opaque UNUSED)
{
	return pgunmap(mem, len);
}

void pgunmap(void *mem, size_t len)
{
	if (munmap(mem, len))
		backtrace_abort();
}

