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

#ifndef LIBVFN_NVME_QUEUE_H
#define LIBVFN_NVME_QUEUE_H

#define NVME_AQ   0
#define NVME_SQES 6
#define NVME_CQES 4

struct nvme_dbbuf {
	void *doorbell;
	void *eventidx;
};

/**
 * struct nvme_cq - Completion Queue
 */
struct nvme_cq {
	/* private: */
	void *vaddr;
	uint64_t iova;

	int id;
	uint16_t head;
	int qsize;
	size_t entry_size;

	/* memory-mapped register */
	void *doorbell;

	struct nvme_dbbuf dbbuf;

	int phase;
	int vector;
};

/**
 * struct nvme_sq - Submission Queue
 */
struct nvme_sq {
	/* private: */
	struct nvme_cq *cq;

	void *vaddr;
	uint64_t iova;

	struct {
		void *vaddr;
		uint64_t iova;
	} pages;

	uint16_t tail, ptail;
	int qsize;
	int id;
	size_t entry_size;

	/* memory-mapped register */
	void *doorbell;

	struct nvme_dbbuf dbbuf;

	/* rq stack */
	struct nvme_rq *rqs;
	struct nvme_rq *rq_top;
};

/**
 * nvme_sq_post - Add a submission queue entry to a submission queue
 * @sq: Submission queue
 * @sqe: Submission queue entry
 *
 * Add a submission queue entry to a submission queue, updating the queue tail
 * pointer in the process.
 */
static inline void nvme_sq_post(struct nvme_sq *sq, const union nvme_cmd *sqe)
{
	memcpy(sq->vaddr + (sq->tail << NVME_SQES), sqe, 1 << NVME_SQES);

	trace_guard(NVME_SQ_POST) {
		trace_emit("sqid %d tail %d\n", sq->id, sq->tail);
	}

	if (++sq->tail == sq->qsize)
		sq->tail = 0;
}

static inline bool __nvme_need_mmio(uint16_t eventidx, uint16_t val, uint16_t old)
{
	return (uint16_t)(val - eventidx) <= (uint16_t)(val - old);
}

static inline int nvme_try_dbbuf(uint16_t v, struct nvme_dbbuf *dbbuf)
{
	uint32_t old, eventidx;

	if (!dbbuf->doorbell)
		return -1;

	/* force submission queue write */
	wmb();

	old = __LOAD_PTR(uint32_t *, dbbuf->doorbell);
	__STORE_PTR(uint32_t *, dbbuf->doorbell, v);

	/* do not reorder the eventidx load with the doorbell store */
	mb();

	eventidx = __LOAD_PTR(uint32_t *, dbbuf->eventidx);

	if (!__nvme_need_mmio((uint16_t)eventidx, v, (uint16_t)old)) {
		trace_guard(NVME_SKIP_MMIO) {
			trace_emit("eventidx %u val %u old %u\n", eventidx, v, old);
		}

		return 0;
	}

	return -1;
}

/**
 * nvme_sq_update_tail - Write the submission queue doorbell
 * @sq: Submission queue
 *
 * Write the queue doorbell if the tail pointer has changed since last written.
 */
static inline void nvme_sq_update_tail(struct nvme_sq *sq)
{
	if (sq->tail == sq->ptail)
		return;

	trace_guard(NVME_SQ_UPDATE_TAIL) {
		trace_emit("sqid %d tail %d\n", sq->id, sq->tail);
	}

	if (nvme_try_dbbuf(sq->tail, &sq->dbbuf)) {
		/* do not reorder queue entry store with doorbell store */
		wmb();

		mmio_write32(sq->doorbell, 0, cpu_to_le32(sq->tail));
	}

	sq->ptail = sq->tail;
}

/**
 * nvme_sq_exec - Post submission queue entry and write the doorbell
 * @sq: Submission queue
 * @sqe: Submission queue entry
 *
 * Combine the effects of nvme_sq_post() and nvme_sq_update_tail().
 */
static inline void nvme_sq_exec(struct nvme_sq *sq, const union nvme_cmd *sqe)
{
	nvme_sq_post(sq, sqe);
	nvme_sq_update_tail(sq);
}

/**
 * nvme_cq_head - Get a pointer to the current completion queue head
 * @cq: Completion queue
 *
 * Return: Pointer to completion queue head entry
 */
static inline struct nvme_cqe *nvme_cq_head(struct nvme_cq *cq)
{
	return (struct nvme_cqe *)(cq->vaddr + (cq->head << NVME_CQES));
}

/**
 * nvme_cq_update_head - Write the completion queue head doorbell
 * @cq: Completion queue
 *
 * Write the completion queue head doorbell.
 */
static inline void nvme_cq_update_head(struct nvme_cq *cq)
{
	trace_guard(NVME_CQ_UPDATE_HEAD) {
		trace_emit("cqid %d head %d\n", cq->id, cq->head);
	}

	if (nvme_try_dbbuf(cq->head, &cq->dbbuf))
		mmio_write32(cq->doorbell, 0, cpu_to_le32(cq->head));
}

/**
 * nvme_cq_spin - Continuously read the top completion queue entry until phase
 *                change
 * @cq: Completion queue
 *
 * Keep reading the current head of the completion queue @cq until the phase
 * changes.
 */
static inline void nvme_cq_spin(struct nvme_cq *cq)
{
	struct nvme_cqe *cqe = nvme_cq_head(cq);

	trace_guard(NVME_CQ_SPIN) {
		trace_emit("cq %d\n", cq->id);
	}

	/*
	 * The compiler can easily prove that the loop never changes the memory
	 * location where the top cqe is read from, so it emits code that checks
	 * it once and then just loops forever.
	 *
	 * Deter the compiler from doing this optimization by forcing a volatile
	 * access with LOAD().
	 */
	while ((le16_to_cpu(LOAD(cqe->sfp)) & 0x1) == cq->phase)
		;
}

/**
 * nvme_cq_get_cqe - Get a pointer to the current completion queue head and
 *                   advance it
 * @cq: Completion queue
 *
 * If the current completion queue head entry is valid (has the right phase),
 * advance the head and return a pointer to it. Otherwise, return NULL and leave
 * the head as-is.
 *
 * Note: Does NOT update the cq head doorbell. See nvme_cq_update_head().
 *
 * Return: If the current completion queue head entry is valid (correct phase),
 * return a pointer to it. Otherwise, return NULL.
 */
static inline struct nvme_cqe *nvme_cq_get_cqe(struct nvme_cq *cq)
{
	struct nvme_cqe *cqe = nvme_cq_head(cq);

	trace_guard(NVME_CQ_GET_CQE) {
		trace_emitrl(1, (uintptr_t)cq, "cq %d\n", cq->id);
	}

	if ((le16_to_cpu(LOAD(cqe->sfp)) & 0x1) == cq->phase)
		return NULL;

	trace_guard(NVME_CQ_GOT_CQE) {
		trace_emit("cq %d cid %" PRIu16 "\n", cq->id, cqe->cid);
	}

	/* prevent load/load reordering between sfp and head */
	dma_rmb();

	if (unlikely(++cq->head == cq->qsize)) {
		cq->head = 0;
		cq->phase ^= 0x1;
	}

	return cqe;
}

/**
 * nvme_cq_get_cqes - Get an exact number of cqes from a completion queue
 * @cq: Completion queue
 * @cqes: Pointer to array of struct cqe to place cqes into
 * @n: number of cqes to reap
 *
 * Continuously spin on @cq and copy @n cqes into @cqes if not NULL.
 *
 * Note: Does NOT update the cq head pointer. See nvme_cq_update_head().
 */
void nvme_cq_get_cqes(struct nvme_cq *cq, struct nvme_cqe *cqes, int n);

/**
 * nvme_cq_wait_cqes - Get an exact number of cqes from a completion queue with
 *                     timeout
 * @cq: Completion queue
 * @cqes: Pointer to array of struct cqe to place cqes into
 * @n: Number of cqes to reap
 * @timeout_ns: Maximum time to wait for CQEs in nanoseconds
 *
 * Continuously poll @cq and copy @n cqes into @cqes if not NULL.
 *
 * Note: Does NOT update the cq head pointer. See nvme_cq_update_head().
 *
 * Return: ``n`` on success. On timeout, returns the number of cqes reaped
 * (i.e., less than ``n``) and sets ``errno``.
 */
int nvme_cq_wait_cqes(struct nvme_cq *cq, struct nvme_cqe *cqes, int n, uint64_t timeout_ns);

#endif /* LIBVFN_NVME_QUEUE_H */
