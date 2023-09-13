/* SPDX-License-Identifier: LGPL-2.1-or-later or MIT */

#ifndef LIBVFN_VFIO_PCI_H
#define LIBVFN_VFIO_PCI_H

#define off_t int

// NOTE: Dummy PROT to satisfy Linux code
enum {
	PROT_READ = 0,
	PROT_WRITE = 1,
};

// NOTE: Faux open for macOS
int vfio_pci_open(struct driverkit_pci_device *pci, const char *bdf);

/**
 * vfio_pci_map_bar - faux mapping in DriverKit
 * @pci: &struct driverkit_pci_device
 * @idx: the vfio region index to map
 * @len: number of bytes to map
 * @offset: offset at which to start mapping
 * @prot: what accesses to permit to the mapped area (see ``man mmap``).
 *
 * Used in place of VFIO map device memory region identified by @idx into virtual memory.
 *
 * Return: On success, returns the virtual memory address mapped. On error,
 * returns ``NULL`` and sets ``errno``.
 */
void *vfio_pci_map_bar(struct driverkit_pci_device *pci, int idx, size_t len, uint64_t offset,
			   int prot);

/**
 * vfio_pci_unmap_bar - faux unmap a region in virtual memory for DriverKit
 * @pci: &struct driverkit_pci_device
 * @idx: the vfio region index to unmap
 * @mem: virtual memory address to unmap
 * @len: number of bytes to unmap
 * @offset: offset at which to start unmapping
 *
 * Used in place of VFIO to unmap the virtual memory address, previously mapped
 * to the vfio device memory region identified by @idx.
 */
void vfio_pci_unmap_bar(struct driverkit_pci_device *pci, int idx, void *mem, size_t len,
			uint64_t offset);

#endif /* LIBVFN_VFIO_PCI_H */
