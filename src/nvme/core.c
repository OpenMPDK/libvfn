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

#include <assert.h>
#include <byteswap.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

#include <sys/mman.h>

#include <linux/vfio.h>

#include <vfn/support/align.h>
#include <vfn/support/compiler.h>
#include <vfn/support/atomic.h>
#include <vfn/support/endian.h>
#include <vfn/support/log.h>
#include <vfn/support/mem.h>
#include <vfn/support/mmio.h>
#include <vfn/trace.h>
#include <vfn/vfio.h>
#include <vfn/pci.h>
#include <vfn/nvme/types.h>
#include <vfn/nvme/queue.h>
#include <vfn/nvme/ctrl.h>
#include <vfn/nvme/rq.h>

#include "ccan/compiler/compiler.h"
#include "ccan/minmax/minmax.h"
#include "ccan/time/time.h"

#include "types.h"

#define cqhdbl(doorbells, qid, dstrd) \
	(doorbells + (2 * qid + 1) * (4 << dstrd))

#define sqtdbl(doorbells, qid, dstrd) \
	(doorbells + (2 * qid) * (4 << dstrd))

enum nvme_ctrl_feature_flags {
	NVME_CTRL_F_ADMINISTRATIVE = 1 << 0,
};

int nvme_configure_cq(struct nvme_ctrl *ctrl, unsigned int qid, unsigned int qsize)
{
	struct nvme_cq *cq = &ctrl->cq[qid];
	uint64_t cap;
	uint8_t dstrd;
	size_t len;

	cap = le64_to_cpu(mmio_read64(ctrl->regs + NVME_REG_CAP));
	dstrd = NVME_FIELD_GET(cap, CAP_DSTRD);

	if (qid > ctrl->config.ncqa) {
		__debug("qid %d invalid; max qid is %d\n", qid, ctrl->config.ncqa);

		errno = EINVAL;
		return -1;
	}

	if (qsize < 2) {
		__debug("qsize must be at least 2\n");
		errno = EINVAL;
		return -1;
	}

	*cq = (struct nvme_cq) {
		.id = qid,
		.qsize = qsize,
		.doorbell = cqhdbl(ctrl->doorbells, qid, dstrd),
		.efd = -1,
	};

	len = pgmapn(&cq->vaddr, qsize, 1 << NVME_CQES);

	if (vfio_map_vaddr(&ctrl->pci.vfio, cq->vaddr, len, &cq->iova)) {
		__debug("failed to map vaddr\n");

		pgunmap(cq->vaddr, len);
		return -1;
	}

	return 0;
}

void nvme_discard_cq(struct nvme_ctrl *ctrl, struct nvme_cq *cq)
{
	size_t len;

	if (!cq->vaddr)
		return;

	len = ALIGN_UP((size_t)cq->qsize << NVME_CQES, __VFN_PAGESIZE);

	if (vfio_unmap_vaddr(&ctrl->pci.vfio, cq->vaddr))
		__debug("failed to unmap vaddr\n");

	pgunmap(cq->vaddr, len);

	memset(cq, 0x0, sizeof(*cq));
}

int nvme_configure_sq(struct nvme_ctrl *ctrl, unsigned int qid, unsigned int qsize,
		      struct nvme_cq *cq, unsigned int UNUSED flags)
{
	struct nvme_sq *sq = &ctrl->sq[qid];
	uint64_t cap;
	uint8_t dstrd;
	ssize_t len;

	cap = le64_to_cpu(mmio_read64(ctrl->regs + NVME_REG_CAP));
	dstrd = NVME_FIELD_GET(cap, CAP_DSTRD);

	if (qid > ctrl->config.nsqa) {
		__debug("qid %d invalid; max qid is %d\n", qid, ctrl->config.nsqa);

		errno = EINVAL;
		return -1;
	}

	if (qsize < 2) {
		__debug("qsize must be at least 2\n");
		errno = EINVAL;
		return -1;
	}

	*sq = (struct nvme_sq) {
		.id = qid,
		.qsize = qsize,
		.doorbell = sqtdbl(ctrl->doorbells, qid, dstrd),
		.cq = cq,
	};

	len = pgmapn(&sq->pages.vaddr, qsize, __VFN_PAGESIZE);
	if (len < 0)
		return -1;

	if (vfio_map_vaddr(&ctrl->pci.vfio, sq->pages.vaddr, len, &sq->pages.iova)) {
		__debug("failed to map vaddr\n");
		goto unmap_pages;
	}

	sq->rqs = zmallocn(qsize - 1, sizeof(struct nvme_rq));
	sq->rq_top = &sq->rqs[qsize - 2];

	for (unsigned int i = 0; i < qsize - 1; i++) {
		struct nvme_rq *rq = &sq->rqs[i];

		rq->sq = sq;
		rq->cid = i;

		rq->page.vaddr = sq->pages.vaddr + (i << __VFN_PAGESHIFT);
		rq->page.iova = sq->pages.iova + (i << __VFN_PAGESHIFT);

		if (i > 0)
			rq->rq_next = &sq->rqs[i - 1];
	}

	len = pgmapn(&sq->vaddr, qsize, 1 << NVME_SQES);
	if (len < 0)
		goto free_sq_rqs;

	if (vfio_map_vaddr(&ctrl->pci.vfio, sq->vaddr, len, &sq->iova)) {
		__debug("failed to map vaddr\n");
		goto unmap_sq;
	}

	return 0;

unmap_sq:
	pgunmap(sq->vaddr, len);
free_sq_rqs:
	free(sq->rqs);
unmap_pages:
	if (vfio_unmap_vaddr(&ctrl->pci.vfio, sq->pages.vaddr))
		__debug("failed to unmap vaddr\n");

	pgunmap(sq->pages.vaddr, (size_t)sq->qsize << __VFN_PAGESHIFT);

	return -1;
}

void nvme_discard_sq(struct nvme_ctrl *ctrl, struct nvme_sq *sq)
{
	size_t len;

	if (!sq->vaddr)
		return;

	len = ALIGN_UP((size_t)sq->qsize << NVME_SQES, __VFN_PAGESIZE);

	if (vfio_unmap_vaddr(&ctrl->pci.vfio, sq->vaddr))
		__debug("failed to unmap vaddr\n");

	pgunmap(sq->vaddr, len);

	free(sq->rqs);

	len = (size_t)sq->qsize << __VFN_PAGESHIFT;

	if (vfio_unmap_vaddr(&ctrl->pci.vfio, sq->pages.vaddr))
		__debug("failed to unmap vaddr\n");

	pgunmap(sq->pages.vaddr, len);

	memset(sq, 0x0, sizeof(*sq));
}

static int nvme_configure_adminq(struct nvme_ctrl *ctrl, unsigned int sq_flags)
{
	int aqa;

	struct nvme_cq *cq = &ctrl->cq[NVME_AQ];
	struct nvme_sq *sq = &ctrl->sq[NVME_AQ];

	if (nvme_configure_cq(ctrl, NVME_AQ, NVME_AQ_QSIZE)) {
		__debug("failed to configure admin completion queue\n");
		return -1;
	}

	if (nvme_configure_sq(ctrl, NVME_AQ, NVME_AQ_QSIZE, cq, sq_flags)) {
		__debug("failed to configure admin submission queue\n");
		goto discard_cq;
	}

	ctrl->adminq.cq = cq;
	ctrl->adminq.sq = sq;

	aqa = NVME_AQ_QSIZE - 1;
	aqa |= aqa << 16;

	mmio_write32(ctrl->regs + NVME_REG_AQA, cpu_to_le32(aqa));
	mmio_hl_write64(ctrl->regs + NVME_REG_ASQ, cpu_to_le64(sq->iova));
	mmio_hl_write64(ctrl->regs + NVME_REG_ACQ, cpu_to_le64(cq->iova));

	return 0;

discard_cq:
	nvme_discard_cq(ctrl, cq);
	return -1;
}

int nvme_create_iocq(struct nvme_ctrl *ctrl, unsigned int qid, unsigned int qsize)
{
	struct nvme_cq *cq = &ctrl->cq[qid];
	union nvme_cmd cmd;

	if (nvme_configure_cq(ctrl, qid, qsize)) {
		__debug("could not configure io completion queue\n");
		return -1;
	}

	cmd.create_cq = (struct nvme_cmd_create_cq) {
		.opcode = NVME_ADMIN_CREATE_CQ,
		.prp1   = cpu_to_le64(cq->iova),
		.qid    = cpu_to_le16(qid),
		.qsize  = cpu_to_le16(qsize - 1),
		.qflags = cpu_to_le16(NVME_Q_PC),
	};

	return nvme_admin(ctrl, &cmd, NULL, 0x0, NULL);
}

int nvme_delete_iocq(struct nvme_ctrl *ctrl, unsigned int qid)
{
	union nvme_cmd cmd;

	nvme_discard_cq(ctrl, &ctrl->cq[qid]);

	cmd.delete_q = (struct nvme_cmd_delete_q) {
		.opcode = NVME_ADMIN_DELETE_CQ,
		.qid = cpu_to_le16(qid),
	};

	return nvme_admin(ctrl, &cmd, NULL, 0x0, NULL);
}

int nvme_create_iosq(struct nvme_ctrl *ctrl, unsigned int qid, unsigned int qsize,
		     struct nvme_cq *cq, unsigned int flags)
{
	struct nvme_sq *sq = &ctrl->sq[qid];
	union nvme_cmd cmd;

	if (nvme_configure_sq(ctrl, qid, qsize, cq, flags)) {
		__debug("could not configure io submission queue\n");
		return -1;
	}

	cmd.create_sq = (struct nvme_cmd_create_sq) {
		.opcode = NVME_ADMIN_CREATE_SQ,
		.prp1   = cpu_to_le64(sq->iova),
		.qid    = cpu_to_le16(qid),
		.qsize  = cpu_to_le16(qsize - 1),
		.qflags = cpu_to_le16(NVME_Q_PC),
		.cqid   = cpu_to_le16(cq->id),
	};

	return nvme_admin(ctrl, &cmd, NULL, 0x0, NULL);
}

int nvme_delete_iosq(struct nvme_ctrl *ctrl, unsigned int qid)
{
	union nvme_cmd cmd;

	nvme_discard_sq(ctrl, &ctrl->sq[qid]);

	cmd.delete_q = (struct nvme_cmd_delete_q) {
		.opcode = NVME_ADMIN_DELETE_SQ,
		.qid = cpu_to_le16(qid),
	};

	return nvme_admin(ctrl, &cmd, NULL, 0x0, NULL);
}

int nvme_create_ioqpair(struct nvme_ctrl *ctrl, unsigned int qid, unsigned int qsize,
			unsigned int flags)
{
	if (nvme_create_iocq(ctrl, qid, qsize)) {
		__debug("could not create io completion queue\n");
		return -1;
	}

	if (nvme_create_iosq(ctrl, qid, qsize, &ctrl->cq[qid], flags)) {
		__debug("could not create io submission queue\n");
		return -1;
	}

	return 0;
}

int nvme_delete_ioqpair(struct nvme_ctrl *ctrl, unsigned int qid)
{
	if (nvme_delete_iosq(ctrl, qid)) {
		__debug("could not delete io submission queue\n");
		return -1;
	}

	if (nvme_delete_iocq(ctrl, qid)) {
		__debug("could not delete io completion queue\n");
		return -1;
	}

	return 0;
}

static int nvme_wait_rdy(struct nvme_ctrl *ctrl, unsigned short rdy)
{
	uint64_t cap;
	uint32_t csts;
	unsigned long timeout_ms;
	struct timeabs deadline;

	cap = le64_to_cpu(mmio_read64(ctrl->regs + NVME_REG_CAP));
	timeout_ms = 500 * (NVME_FIELD_GET(cap, CAP_TO) + 1);
	deadline = timeabs_add(time_now(), time_from_msec(timeout_ms));

	do {
		if (time_after(time_now(), deadline)) {
			__debug("timed out\n");

			errno = ETIMEDOUT;
			return -1;
		}

		csts = le32_to_cpu(mmio_read32(ctrl->regs + NVME_REG_CSTS));
	} while (NVME_FIELD_GET(csts, CSTS_RDY) != rdy);

	return 0;
}

int nvme_enable(struct nvme_ctrl *ctrl)
{
	uint8_t css;
	uint32_t cc;
	uint64_t cap;

	cap = le64_to_cpu(mmio_read64(ctrl->regs + NVME_REG_CAP));
	css = NVME_FIELD_GET(cap, CAP_CSS);

	cc =
		NVME_FIELD_SET(__VFN_PAGESHIFT - 12, CC_MPS) |
		NVME_FIELD_SET(NVME_CC_AMS_RR,	     CC_AMS) |
		NVME_FIELD_SET(NVME_CC_SHN_NONE,     CC_SHN) |
		NVME_FIELD_SET(NVME_SQES,            CC_IOSQES) |
		NVME_FIELD_SET(NVME_CQES,            CC_IOCQES) |
		NVME_FIELD_SET(0x1,                  CC_EN);

	if (css & NVME_CAP_CSS_CSI)
		cc |= NVME_FIELD_SET(NVME_CC_CSS_CSI, CC_CSS);
	else if (css & NVME_CAP_CSS_ADMIN)
		cc |= NVME_FIELD_SET(NVME_CC_CSS_ADMIN, CC_CSS);
	else
		cc |= NVME_FIELD_SET(NVME_CC_CSS_NVM, CC_CSS);

	mmio_write32(ctrl->regs + NVME_REG_CC, cpu_to_le32(cc));

	return nvme_wait_rdy(ctrl, 1);
}

int nvme_reset(struct nvme_ctrl *ctrl)
{
	uint32_t cc;

	cc = le32_to_cpu(mmio_read32(ctrl->regs + NVME_REG_CC));
	mmio_write32(ctrl->regs + NVME_REG_CC, cpu_to_le32(cc & 0xfe));

	return nvme_wait_rdy(ctrl, 0);
}

int nvme_init(struct nvme_ctrl *ctrl, const char *bdf, const struct nvme_ctrl_opts *opts)
{
	unsigned long long classcode;
	uint64_t cap;
	uint8_t mpsmin;

	union nvme_cmd cmd = {};
	struct nvme_cqe cqe;

	unsigned int sq_flags = 0x0;

	if (opts)
		memcpy((struct nvme_ctrl_opts *)&ctrl->opts, opts, sizeof(*opts));
	else
		memcpy((struct nvme_ctrl_opts *)&ctrl->opts, &nvme_ctrl_opts_default,
		       sizeof(*opts));

	if (pci_device_info_get_ull(bdf, "class", &classcode)) {
		__debug("could not get device class code\n");
		return -1;
	}

	__log(LOG_INFO, "pci class code is 0x%06llx\n", classcode);

	if ((classcode & 0xffff00) != 0x010800) {
		__debug("%s is not an NVMe device\n", bdf);
		errno = EINVAL;
		return -1;
	}

	if ((classcode & 0xff) == 0x03)
		ctrl->flags = NVME_CTRL_F_ADMINISTRATIVE;

	if (vfio_pci_open(&ctrl->pci, bdf))
		return -1;

	ctrl->regs = vfio_pci_map_bar(&ctrl->pci, 0, 0x1000, 0, PROT_READ | PROT_WRITE);
	if (!ctrl->regs) {
		__debug("could not map controller registersn\n");
		return -1;
	}

	cap = le64_to_cpu(mmio_read64(ctrl->regs + NVME_REG_CAP));
	mpsmin = NVME_FIELD_GET(cap, CAP_MPSMIN);

	if (((12 + mpsmin) >> 12) > __VFN_PAGESIZE) {
		__debug("controller minimum page size too large\n");
		errno = EINVAL;
		return -1;
	}

	if (nvme_reset(ctrl)) {
		__debug("could not reset controller\n");
		return -1;
	}

	/* map admin queue doorbells */
	ctrl->doorbells = vfio_pci_map_bar(&ctrl->pci, 0, 0x1000, 0x1000, PROT_WRITE);
	if (!ctrl->doorbells) {
		__debug("could not map doorbells\n");
		return -1;
	}

	/* +2 because nsqr/ncqr are zero-based values and do not account for the admin queue */
	ctrl->sq = zmallocn(ctrl->opts.nsqr + 2, sizeof(struct nvme_sq));
	ctrl->cq = zmallocn(ctrl->opts.ncqr + 2, sizeof(struct nvme_cq));

	if (nvme_configure_adminq(ctrl, sq_flags)) {
		__debug("could not configure admin queue\n");
		return -1;
	}

	if (nvme_enable(ctrl)) {
		__debug("could not enable controller\n");
		return -1;
	}

	if (ctrl->flags & NVME_CTRL_F_ADMINISTRATIVE)
		return 0;

	cmd = (union nvme_cmd) {
		.opcode = NVME_ADMIN_SET_FEATURES,
		.cid = 0x1,
	};

	cmd.features.fid = NVME_FEAT_FID_NUM_QUEUES;
	cmd.features.cdw11 = cpu_to_le32(
		NVME_FIELD_SET(ctrl->opts.nsqr, FEAT_NRQS_NSQR) |
		NVME_FIELD_SET(ctrl->opts.ncqr, FEAT_NRQS_NCQR));

	if (nvme_admin(ctrl, &cmd, NULL, 0x0, &cqe))
		return -1;

	ctrl->config.nsqa = min_t(uint16_t, ctrl->opts.nsqr,
				  NVME_FIELD_GET(le32_to_cpu(cqe.dw0), FEAT_NRQS_NSQR));
	ctrl->config.ncqa = min_t(uint16_t, ctrl->opts.ncqr,
				  NVME_FIELD_GET(le32_to_cpu(cqe.dw0), FEAT_NRQS_NCQR));

	return 0;
}

void nvme_close(struct nvme_ctrl *ctrl)
{
	for (int i = 0; i < ctrl->opts.nsqr + 2; i++)
		nvme_discard_sq(ctrl, &ctrl->sq[i]);

	free(ctrl->sq);

	for (int i = 0; i < ctrl->opts.ncqr + 2; i++)
		nvme_discard_cq(ctrl, &ctrl->cq[i]);

	free(ctrl->cq);

	vfio_pci_unmap_bar(&ctrl->pci, 0, ctrl->regs, 0x1000, 0);
	vfio_pci_unmap_bar(&ctrl->pci, 0, ctrl->doorbells, 0x1000, 0x1000);

	vfio_close(&ctrl->pci.vfio);
}

int nvme_aen_enable(struct nvme_ctrl *ctrl, cqe_handler handler)
{
	struct nvme_rq *rq;
	union nvme_cmd cmd = { .opcode = NVME_ADMIN_ASYNC_EVENT };

	rq = nvme_rq_acquire_atomic(ctrl->adminq.sq);
	if (!rq) {
		errno = EBUSY;
		return -1;
	}

	nvme_rq_prep_cmd(rq, &cmd);

	cmd.cid |= NVME_CID_AER;
	rq->opaque = handler;

	nvme_sq_exec(ctrl->adminq.sq, &cmd);

	return 0;
}

void nvme_aen_handle(struct nvme_ctrl *ctrl, struct nvme_cqe *cqe)
{
	struct nvme_rq *rq;
	union nvme_cmd cmd = { .opcode = NVME_ADMIN_ASYNC_EVENT };

	assert(cqe->cid & NVME_CID_AER);

	cqe->cid &= ~NVME_CID_AER;

	rq = nvme_rq_from_cqe(ctrl, cqe);

	if (rq->opaque) {
		cqe_handler h = rq->opaque;

		h(cqe);
	} else {
		uint32_t dw0 = le32_to_cpu(cqe->dw0);

		__log(LOG_INFO, "unhandled aen 0x%" PRIx32 " (type 0x%x info 0x%x lid 0x%x)\n",
		      dw0, NVME_AEN_TYPE(dw0), NVME_AEN_INFO(dw0), NVME_AEN_LID(dw0));
	}

	nvme_rq_prep_cmd(rq, &cmd);

	cmd.cid |= NVME_CID_AER;

	nvme_rq_exec(rq, &cmd);
}

int nvme_oneshot(struct nvme_ctrl *ctrl, struct nvme_sq *sq, void *sqe, void *buf, size_t len,
		 void *cqe_copy)
{
	struct nvme_cqe cqe;
	struct nvme_rq *rq;
	uint64_t iova;
	int ret = 0;

	rq = nvme_rq_acquire_atomic(sq);
	if (!rq)
		return -1;

	if (buf) {
		ret = vfio_map_vaddr_ephemeral(&ctrl->pci.vfio, buf, len, &iova);
		if (ret)
			goto release_rq;

		nvme_rq_map_prp(rq, sqe, iova, len);
	}

	nvme_rq_exec(rq, sqe);

	while (nvme_rq_poll(rq, &cqe) < 0 && errno == EAGAIN) {
		if (sq->id == NVME_AQ && (cqe.cid & NVME_CID_AER)) {
			nvme_aen_handle(ctrl, &cqe);
			continue;
		}

		__log(LOG_ERROR, "SPURIOUS CQE (cq %" PRIu16 " cid %" PRIu16 ")\n",
		      rq->sq->cq->id, cqe.cid);
	}

	if (cqe_copy)
		memcpy(cqe_copy, &cqe, 1 << NVME_CQES);

	if (buf)
		ret = vfio_unmap_ephemeral_iova(&ctrl->pci.vfio, len, iova);

release_rq:
	nvme_rq_release_atomic(rq);

	return ret;
}

int nvme_admin(struct nvme_ctrl *ctrl, void *sqe, void *buf, size_t len, void *cqe_copy)
{
	return nvme_oneshot(ctrl, ctrl->adminq.sq, sqe, buf, len, cqe_copy);
}
