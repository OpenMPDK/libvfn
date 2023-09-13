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

#ifndef LIBVFN_IOMMU_DMA_H
#define LIBVFN_IOMMU_DMA_H

/**
 * enum iommu_map_flags - Flags for DMA mapping
 * @IOMMU_MAP_FIXED_IOVA: If cleared, an appropriate IOVA will be allocated
 * @IOMMU_MAP_EPHEMERAL: If set, the mapping is considered temporary
 * @IOMMU_MAP_NOWRITE: DMA is not allowed to write to this mapping
 * @IOMMU_MAP_NOREAD: DMA is not allowed to read from this mapping
 *
 * IOMMU_MAP_EPHEMERAL may change how the iova is allocated. I.e., currently,
 * the vfio-based backend will allocate an IOVA from a reserved range of 64k.
 * The iommufd-based backend has no such restrictions.
 */
enum iommu_map_flags {
	IOMMU_MAP_FIXED_IOVA	= 1 << 0,
	IOMMU_MAP_EPHEMERAL	= 1 << 1,
	IOMMU_MAP_NOWRITE	= 1 << 2,
	IOMMU_MAP_NOREAD	= 1 << 3,
};

#ifndef __APPLE__
/**
 * iommu_map_vaddr - Map a virtual memory address to an I/O virtual address
 * @ctx: &struct iommu_ctx
 * @vaddr: virtual memory address to map
 * @len: number of bytes to map
 * @iova: output parameter for mapped I/O virtual address
 * @flags: combination of enum iommu_map_flags
 *
 * Allocate an I/O virtual address (iova) and program the IOMMU to create a
 * mapping to the virtual memory address in @vaddr. If @iova is not ``NULL``,
 * store the allocated iova in the pointee.
 *
 * If @vaddr falls within an already mapped area, calculate the corresponding
 * iova instead.
 *
 * Note that, for the vfio backend, the allocated IOVA is not recycled when the
 * mapping is removed; the IOVA will never be allocated again.
 *
 * Return: ``0`` on success, ``-1`` on error and sets ``errno``.
 */
int iommu_map_vaddr(struct iommu_ctx *ctx, void *vaddr, size_t len, uint64_t *iova,
		    unsigned long flags);
#endif
int _iommu_map_vaddr(struct iommu_ctx *ctx, void *vaddr, size_t len, uint64_t *iova,
		    unsigned long flags, void *opaque);

/**
 * iommu_unmap_vaddr - Unmap a virtual memory address in the IOMMU
 * @ctx: &struct iommu_ctx
 * @vaddr: virtual memory address to unmap
 * @len: output parameter for length of mapping
 *
 * Remove the mapping associated with @vaddr. This can only be used with
 * mappings created using iommu_map_vaddr(). If @len is not NULL, the length of
 * the mapping will be written to the pointee.
 *
 * Return: ``0`` on success, ``-1`` on error and sets ``errno``.
 */
int iommu_unmap_vaddr(struct iommu_ctx *ctx, void *vaddr, size_t *len);

/**
 * iommu_unmap_all - Unmap all virtual memory address in the IOMMU
 * @ctx: &struct iommu_ctx
 *
 * Remove all mappings.
 *
 * Return: ``0`` on success, ``-1`` on error and sets ``errno``.
 */
int iommu_unmap_all(struct iommu_ctx *ctx);

#ifndef __APPLE__
#ifndef IOMMU_IOAS_IOVA_RANGES
struct iommu_iova_range {
	__aligned_u64 start;
	__aligned_u64 last;
};
#endif
#endif

/**
 * iommu_translate_vaddr - Translate a virtual address into an iova
 * @ctx: &struct iommu_ctx
 * @vaddr: virtual memory address to translate
 * @iova: output parameter
 *
 * Use the iova map within the iommu context to lookup and translate the given
 * virtual address into an I/O virtual address.
 *
 * Return: ``true`` on success, ``false`` if no mapping was found.
 */
bool iommu_translate_vaddr(struct iommu_ctx *ctx, void *vaddr, uint64_t *iova);

#ifndef __APPLE__
/**
 * iommu_get_iova_ranges - Get iova ranges
 * @ctx: &struct iommu_ctx
 * @ranges: output parameter
 *
 * Store the address of an array into @ranges and return the number of elements.
 *
 * Return: the number of elements in the array pointed to.
 */
int iommu_get_iova_ranges(struct iommu_ctx *ctx, struct iommu_iova_range **ranges);
#endif

#endif /* LIBVFN_IOMMU_DMA_H */
