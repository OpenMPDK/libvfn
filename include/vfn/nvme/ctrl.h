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

#ifndef LIBVFN_NVME_CTRL_H
#define LIBVFN_NVME_CTRL_H

#define NVME_CTRL_MPS 0

/**
 * struct nvme_ctrl_opts - NVMe controller options
 * @nsqr: number of submission queues to request
 * @ncqr: number of completion queues to request
 * @quirks: quirks to apply
 *
 * Note: @nsqr and @ncqr are zeroes based values.
 */
struct nvme_ctrl_opts {
	int nsqr, ncqr;
#define NVME_QUIRK_BROKEN_DBBUF (1 << 0)
	unsigned int quirks;
};

static const struct nvme_ctrl_opts nvme_ctrl_opts_default = {
	.nsqr = 63, .ncqr = 63,
	.quirks = 0x0,
};

/**
 * struct nvme_ctrl - NVMe Controller
 * @sq: submission queues
 * @cq: completion queues
 */
struct nvme_ctrl {
	/**
	 * @pci: vfio pci device state
	 */
	struct vfio_pci_device pci;

	/**
	 * @serial: Serial number from controller
	 */
	char serial[20];

	/**
	 * @regs: Controller Configuration (``MBAR.CC``) registers
	 */
	void *regs;

	struct nvme_sq *sq;
	struct nvme_cq *cq;

	/**
	 * @adminq: Admin queue pair
	 */
	struct {
		struct nvme_sq *sq;
		struct nvme_cq *cq;
	} adminq;

	/**
	 * @doorbells: mapped doorbell registers
	 */
	void *doorbells;

	/**
	 * @dbbuf: doorbell buffers
	 */
	struct {
		void *doorbells;
		void *eventidxs;
	} dbbuf;

	/**
	 * @opts: controller options
	 */
	struct nvme_ctrl_opts opts;

	/**
	 * @config: cached run-time controller configuration
	 */
	struct {
		int nsqa, ncqa;
		int mqes;
		int mps;
	} config;

	/* private: internal */
	unsigned long flags;
};

/**
 * nvme_init - Initialize controller
 * @ctrl: Controller to initialize
 * @bdf: PCI device identifier ("bus:device:function")
 * @opts: Controller configuration options
 *
 * Initialize the controller identified by @bdf. The controller is ready for use
 * after a successful call to this.
 *
 * See &struct nvme_ctrl_opts for configurable options.
 *
 * Return: ``0`` on success, ``-1`` on error and sets ``errno``.
 */
int nvme_init(struct nvme_ctrl *ctrl, const char *bdf, const struct nvme_ctrl_opts *opts);

/**
 * nvme_close - Close a controller
 * @ctrl: Controller to close
 *
 * Uninitialize the controller, deleting any i/o queues and releasing VFIO
 * resources.
 */
void nvme_close(struct nvme_ctrl *ctrl);

/**
 * nvme_reset - Reset controller
 * @ctrl: Controller to reset
 *
 * Reset the controller referenced by @ctrl.
 *
 * Return: See nvme_wait_rdy().
 */
int nvme_reset(struct nvme_ctrl *ctrl);

/**
 * nvme_enable - Enable controller
 * @ctrl: Controller to enable
 *
 * Enable the controller referenced by @ctrl.
 *
 * Return: See nvme_wait_rdy().
 */
int nvme_enable(struct nvme_ctrl *ctrl);

/**
 * nvme_create_iocq - Create an I/O Completion Queue
 * @ctrl: Controller reference
 * @qid: Queue identifier
 * @qsize: Queue size
 * @vector: interrupt vector
 *
 * Create an I/O Completion Queue on @ctrl with identifier @qid and size @qsize.
 * Set @vector to -1 to disable interrupts. If you associate an interrupt
 * vector, you need to use vfio_set_irq() to associate the vector with an
 * eventfd.
 *
 * **Note** that one slot in the queue is reserved for the full queue condition.
 * So, if a queue command depth of ``N`` is required, qsize should be ``N + 1``.
 *
 * Return: On success, returns ``0``. On error, returns ``-1`` and sets
 * ``errno``.
 */
int nvme_create_iocq(struct nvme_ctrl *ctrl, int qid, int qsize, int vector);

/**
 * nvme_delete_iocq - Delete an I/O Completion Queue
 * @ctrl: See &struct nvme_ctrl
 * @qid: Queue identifier
 *
 * Delete the I/O Completion queue associated with @qid.
 *
 * Return: On success, returns ``0``. On error, returns ``-1`` and sets
 * ``errno``.
 */
int nvme_delete_iocq(struct nvme_ctrl *ctrl, int qid);

/**
 * nvme_create_iosq - Create an I/O Submission Queue
 * @ctrl: Controller reference
 * @qid: Queue identifier
 * @qsize: Queue size
 * @cq: Associated I/O Completion Queue
 * @flags: See &enum nvme_create_iosq_flags
 *
 * Create an I/O Submission Queue on @ctrl with identifier @qid, queue size
 * @qsize and associated with I/O Completion Queue @cq. @flags may be used to
 * modify the behavior (see &enum nvme_create_iosq_flags).
 *
 * **Note** that one slot in the queue is reserved for the full queue condition.
 * So, if a queue command depth of ``N`` is required, qsize should be ``N + 1``.
 *
 * Return: On success, returns ``0``. On error, returns ``-1`` and sets
 * ``errno``.
 */
int nvme_create_iosq(struct nvme_ctrl *ctrl, int qid, int qsize,
		     struct nvme_cq *cq, unsigned long flags);

/**
 * nvme_delete_iosq - Delete an I/O Submission Queue
 * @ctrl: See &struct nvme_ctrl
 * @qid: Queue identifier
 *
 * Delete the I/O Submission queue associated with @qid.
 *
 * Return: On success, returns ``0``. On error, returns ``-1`` and sets
 * ``errno``.
 */
int nvme_delete_iosq(struct nvme_ctrl *ctrl, int qid);

/**
 * nvme_create_ioqpair - Create an I/O Completion/Submission Queue Pair
 * @ctrl: Controller reference
 * @qid: Queue identifier
 * @qsize: Queue size
 * @vector: Completion queue interrupt vector
 * @flags: See &enum nvme_create_iosq_flags
 *
 * Create both an I/O Submission Queue and an I/O Completion Queue with the same
 * queue identifier, queue size. @flags may be used to modify the behavior of
 * the submission queu (see &enum nvme_create_iosq_flags).
 *
 * **Note** that one slot in the queue is reserved for the full queue condition.
 * So, if a queue command depth of ``N`` is required, qsize should be ``N + 1``.
 *
 * Return: On success, returns ``0``. On error, returns ``-1`` and sets
 * ``errno``.
 */
int nvme_create_ioqpair(struct nvme_ctrl *ctrl, int qid, int qsize, int vector,
			unsigned long flags);

/**
 * nvme_delete_ioqpair - Delete an I/O Completion/Submission Queue Pair
 * @ctrl: See &struct nvme_ctrl
 * @qid: Queue identifier
 *
 * Delete both the I/O Submission and Completion queues associated with @qid.
 *
 * Return: On success, returns ``0``. On error, returns ``-1`` and sets
 * ``errno``.
 */
int nvme_delete_ioqpair(struct nvme_ctrl *ctrl, int qid);

#endif /* LIBVFN_NVME_CTRL_H */
