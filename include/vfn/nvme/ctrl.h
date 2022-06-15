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

struct nvme_ctrl_opts {
	uint16_t nsqr, ncqr;
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
	struct vfio_pci_state pci;

	/**
	 * @regs: Controller Configuration (``MBAR.CC``) registers
	 */
	void *regs;

	struct nvme_sq *sq;
	struct nvme_cq *cq;

	/**
	 * @adminq: Admin queue pair
	 */
	struct nvme_qpair adminq;

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
	const struct nvme_ctrl_opts opts;

	/**
	 * @config: cached run-time controller configuration
	 */
	struct {
		uint16_t nsqa, ncqa;
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
 *
 * Create an I/O Completion Queue on @ctrl with identifier @qid and size @qsize.
 *
 * **Note** that one slot in the queue is reserved for the full queue condition.
 * So, if a queue command depth of ``N`` is required, qsize should be ``N + 1``.
 *
 * Return: On success, returns ``0``. On error, returns ``-1`` and sets
 * ``errno``.
 */
int nvme_create_iocq(struct nvme_ctrl *ctrl, unsigned int qid, unsigned int qsize);

/**
 * nvme_configure_cq - Configure completion queue
 * @ctrl: See &struct nvme_ctrl
 * @qid: Queue identifier
 * @qsize: Queue qsize
 *
 * Configure the completion queue associated with @qid and initialize its size.
 * This does NOT create the queue on the controller. This is a low-level
 * primitive that may be used if, for some reason, nvme_create_iocq() cannot be
 * used.
 *
 * **Note** that one slot in the queue is reserved for the full queue condition.
 * So, if a queue command depth of ``N`` is required, qsize should be ``N + 1``.
 *
 * Return: On success, returns ``0``. On error, returns ``-1`` and sets
 * ``errno``.
 */
int nvme_configure_cq(struct nvme_ctrl *ctrl, unsigned int qid, unsigned int qsize);

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
int nvme_delete_iocq(struct nvme_ctrl *ctrl, unsigned int qid);

/*
 * nvme_discard_cq - Discard a completion queue
 * @ctrl: See &struct nvme_ctrl
 * @cq: Completion queue
 *
 * Discard the @cq completion queue and free its resources. This does NOT delete
 * the queue on the controller. This is a low-level primitive that may be used
 * if, for some reason, nvme_delete_iocq() cannot be used.
 *
 * Return: On success, returns ``0``. On error, returns ``-1`` and sets
 * ``errno``.
 */
void nvme_discard_cq(struct nvme_ctrl *ctrl, struct nvme_cq *cq);

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
int nvme_create_iosq(struct nvme_ctrl *ctrl, unsigned int qid, unsigned int qsize,
		     struct nvme_cq *cq, unsigned int flags);

/**
 * nvme_configure_sq - Configure submission queue
 * @ctrl: See &struct nvme_ctrl
 * @qid: Queue identifier
 * @qsize: Queue qsize
 * @cq: Associated completion queue
 * @flags: currently unused
 *
 * Configure the submission queue associated with @qid, initialize its size and
 * setup request trackers. This does NOT create the queue on the controller.
 * This is a low-level primitive that may be used if (for some reason)
 * nvme_create_iosq() cannot be used.
 *
 * **Note** that one slot in the queue is reserved for the full queue condition.
 * So, if a queue command depth of ``N`` is required, qsize should be ``N + 1``.
 *
 * Return: On success, returns ``0``. On error, returns ``-1`` and sets
 * ``errno``.
 */
int nvme_configure_sq(struct nvme_ctrl *ctrl, unsigned int qid, unsigned int qsize,
		      struct nvme_cq *cq, unsigned int flags);

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
int nvme_delete_iosq(struct nvme_ctrl *ctrl, unsigned int qid);

/**
 * nvme_discard_sq - Discard a submission queue
 * @ctrl: See &struct nvme_ctrl
 * @sq: Submission queue
 *
 * Discard the @sq submission queue and free its resources. This does NOT delete
 * the queue on the controller. This is a low-level primitive that may be used
 * if, for some reason, nvme_delete_iosq() cannot be used.
 *
 * Return: On success, returns ``0``. On error, returns ``-1`` and sets
 * ``errno``.
 */
void nvme_discard_sq(struct nvme_ctrl *ctrl, struct nvme_sq *sq);

/**
 * nvme_create_ioqpair - Create an I/O Completion/Submission Queue Pair
 * @ctrl: Controller reference
 * @qid: Queue identifier
 * @qsize: Queue size
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
int nvme_create_ioqpair(struct nvme_ctrl *ctrl, unsigned int qid, unsigned int qsize,
			unsigned int flags);

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
int nvme_delete_ioqpair(struct nvme_ctrl *ctrl, unsigned int qid);

/**
 * nvme_aen_enable - Enable handling of AENs
 * @ctrl: Controller reference
 * @handler: A &cqe_handler_fn
 *
 * Issue an Asynchronous Event Request command and register @handler to be
 * called when the AER completes.
 *
 * Return: On success, returns ``0``. On error, returns ``-1`` and sets
 * ``errno``.
 */
int nvme_aen_enable(struct nvme_ctrl *ctrl, cqe_handler handler);

/**
 * nvme_aen_handle - Handle an asynchronous event notification
 * @ctrl: Controller reference
 * @cqe: Completion queue entry for the AEN
 *
 * Handle an AEN by calling the handler previously registered with
 * nvme_aen_enable() and post a fresh Asynchronous Event Request command.
 */
void nvme_aen_handle(struct nvme_ctrl *ctrl, struct nvme_cqe *cqe);

/**
 * nvme_oneshot - Submit a command and wait for completion
 * @ctrl: Controller reference
 * @sq: Submission queue
 * @sqe: Submission queue entry
 * @buf: Command payload
 * @len: Command payload length
 * @cqe: Completion queue entry to fill
 *
 * Submit a command and wait for completion in a synchronous manner. If a
 * spurious completion queue entry is posted (i.e., the command identifier is
 * different from the one set in @sqe), the CQE is ignored and an error message
 * is logged. However, if waiting on the admin completion queue and the command
 * identifier indicates that this is an AEN, pass the cqe to a registered
 * handler (if any).
 *
 * **Note**: As the name indicate, this function should only be used for "one
 * shot" commands where it is guaranteed that no (non AEN) spurious CQEs are
 * posted on the completion queue.
 *
 * Return: On success, returns ``0``. On error, returns ``-1`` and sets
 * ``errno``.
 */
int nvme_oneshot(struct nvme_ctrl *ctrl, struct nvme_sq *sq, void *sqe, void *buf, size_t len,
		 void *cqe);

/**
 * nvme_admin - Submit an Admin command and wait for completion
 * @ctrl: See &struct nvme_ctrl
 * @sqe: Submission queue entry
 * @buf: Command payload
 * @len: Command payload length
 * @cqe: Completion queue entry to fill
 *
 * Shortcut for nvme_oneshot(), submitting to the admin submission queue.
 *
 * Return: On success, returns ``0``. On error, returnes ``-1`` and sets
 * ``errno``.
 */
int nvme_admin(struct nvme_ctrl *ctrl, void *sqe, void *buf, size_t len, void *cqe);

#endif /* LIBVFN_NVME_CTRL_H */
