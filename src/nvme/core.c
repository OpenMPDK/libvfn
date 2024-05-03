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

#define log_fmt(fmt) "nvme/core: " fmt

#include <vfn/nvme.h>
#include <vfn/pci.h>
#include <vfn/iommu/dma.h>

#include "ccan/compiler/compiler.h"
#include "ccan/minmax/minmax.h"
#ifndef __APPLE__
#include "ccan/time/time.h"
#else
#include <time.h>
int errno;
#endif

#include "types.h"

#define cqhdbl(doorbells, qid, dstrd) \
	(doorbells + (2 * qid + 1) * (4 << dstrd))

#define sqtdbl(doorbells, qid, dstrd) \
	(doorbells + (2 * qid) * (4 << dstrd))

enum nvme_ctrl_feature_flags {
	NVME_CTRL_F_ADMINISTRATIVE = 1 << 0,
};

static int nvme_configure_cq(struct nvme_ctrl *ctrl, int qid, int qsize, int vector)
{
	struct nvme_cq *cq = &ctrl->cq[qid];
	uint64_t cap;
	uint8_t dstrd;
	size_t len;
	void *opaque;

	cap = le64_to_cpu(mmio_read64(ctrl->regs, NVME_REG_CAP));
	dstrd = NVME_FIELD_GET(cap, CAP_DSTRD);

	if (qid && qid > ctrl->config.ncqa + 1) {
		log_debug("qid %d invalid; max qid is %d\n", qid, ctrl->config.ncqa + 1);

		errno = EINVAL;
		return -1;
	}

	if (qsize < 2) {
		log_debug("qsize must be at least 2\n");
		errno = EINVAL;
		return -1;
	}

	if (qid && qsize > ctrl->config.mqes + 1) {
		log_debug("qsize %d invalid; max qsize is %d\n", qsize, ctrl->config.mqes + 1);
	}

	*cq = (struct nvme_cq) {
		.id = qid,
		.qsize = qsize,
		.doorbell = cqhdbl(ctrl->doorbells, qid, dstrd),
		.vector = vector,
	};

	if (ctrl->dbbuf.doorbells) {
		cq->dbbuf.doorbell = cqhdbl(ctrl->dbbuf.doorbells, qid, dstrd);
		cq->dbbuf.eventidx = cqhdbl(ctrl->dbbuf.eventidxs, qid, dstrd);
	}

	len = __pgmapn(&cq->vaddr, qsize, 1 << NVME_CQES, &opaque);

	if (_iommu_map_vaddr(__iommu_ctx(ctrl), cq->vaddr, len, &cq->iova, 0x0, opaque)) {
		log_debug("failed to map vaddr\n");

		__pgunmap(cq->vaddr, len, opaque);
		return -1;
	}

	return 0;
}

static void nvme_discard_cq(struct nvme_ctrl *ctrl, struct nvme_cq *cq)
{
	size_t len;

	if (!cq->vaddr)
		return;

	if (iommu_unmap_vaddr(__iommu_ctx(ctrl), cq->vaddr, &len))
		log_debug("failed to unmap vaddr\n");

	__pgunmap(cq->vaddr, len, cq->vaddr_opaque);

	if (ctrl->dbbuf.doorbells) {
		__STORE_PTR(uint32_t *, cq->dbbuf.doorbell, 0);
		__STORE_PTR(uint32_t *, cq->dbbuf.eventidx, 0);
	}

	memset(cq, 0x0, sizeof(*cq));
}

static int nvme_configure_sq(struct nvme_ctrl *ctrl, int qid, int qsize,
			     struct nvme_cq *cq, unsigned long UNUSED flags)
{
	struct nvme_sq *sq = &ctrl->sq[qid];
	uint64_t cap;
	uint8_t dstrd;
	ssize_t len;

	cap = le64_to_cpu(mmio_read64(ctrl->regs, NVME_REG_CAP));
	dstrd = NVME_FIELD_GET(cap, CAP_DSTRD);

	if (qid && qid > ctrl->config.nsqa + 1) {
		log_debug("qid %d invalid; max qid is %d\n", qid, ctrl->config.nsqa + 1);

		errno = EINVAL;
		return -1;
	}

	if (qsize < 2) {
		log_debug("qsize must be at least 2\n");
		errno = EINVAL;
		return -1;
	}

	if (qid && qsize > ctrl->config.mqes + 1) {
		log_debug("qsize %d invalid; max qsize is %d\n", qsize, ctrl->config.mqes + 1);
	}

	*sq = (struct nvme_sq) {
		.id = qid,
		.qsize = qsize,
		.doorbell = sqtdbl(ctrl->doorbells, qid, dstrd),
		.cq = cq,
	};

	if (ctrl->dbbuf.doorbells) {
		sq->dbbuf.doorbell = sqtdbl(ctrl->dbbuf.doorbells, qid, dstrd);
		sq->dbbuf.eventidx = sqtdbl(ctrl->dbbuf.eventidxs, qid, dstrd);
	}

	/*
	 * Use ctrl->config.mps instead of host page size, as we have the
	 * opportunity to pack the allocations.
	 */
	len = __pgmapn(&sq->pages.vaddr, qsize, __mps_to_pagesize(ctrl->config.mps),
		&sq->pages.vaddr_opaque);

	if (len < 0)
		return -1;

	if (_iommu_map_vaddr(__iommu_ctx(ctrl), sq->pages.vaddr, len, &sq->pages.iova, 0x0,
		sq->pages.vaddr_opaque)) {
		log_debug("failed to map vaddr\n");
		goto unmap_pages;
	}

	sq->rqs = znew_t(struct nvme_rq, qsize - 1);
	sq->rq_top = &sq->rqs[qsize - 2];

	for (int i = 0; i < qsize - 1; i++) {
		struct nvme_rq *rq = &sq->rqs[i];

		rq->sq = sq;
		rq->cid = (uint16_t)i;

		rq->page.vaddr = (void *)((uintptr_t)sq->pages.vaddr +
			((uint64_t)i << __mps_to_pageshift(ctrl->config.mps)));
		rq->page.iova = sq->pages.iova +
			((uint64_t)i << __mps_to_pageshift(ctrl->config.mps));

		if (i > 0)
			rq->rq_next = &sq->rqs[i - 1];
	}

	len = __pgmapn(&sq->vaddr, qsize, 1 << NVME_SQES, &sq->vaddr_opaque);
	if (len < 0)
		goto free_sq_rqs;

	if (_iommu_map_vaddr(__iommu_ctx(ctrl), sq->vaddr, len, &sq->iova, 0x0, sq->vaddr_opaque)) {
		log_debug("failed to map vaddr\n");
		goto unmap_sq;
	}

	return 0;

unmap_sq:
	__pgunmap(sq->vaddr, len, sq->vaddr_opaque);
free_sq_rqs:
	free(sq->rqs);
unmap_pages:
	if (iommu_unmap_vaddr(__iommu_ctx(ctrl), sq->pages.vaddr, (size_t *)&len))
		log_debug("failed to unmap vaddr\n");

	__pgunmap(sq->pages.vaddr, len, sq->pages.vaddr_opaque);

	return -1;
}

static void nvme_discard_sq(struct nvme_ctrl *ctrl, struct nvme_sq *sq)
{
	size_t len;

	if (!sq->vaddr)
		return;

	if (iommu_unmap_vaddr(__iommu_ctx(ctrl), sq->vaddr, &len))
		log_debug("failed to unmap vaddr\n");

	__pgunmap(sq->vaddr, len, sq->vaddr_opaque);

	free(sq->rqs);

	if (iommu_unmap_vaddr(__iommu_ctx(ctrl), sq->pages.vaddr, &len))
		log_debug("failed to unmap vaddr\n");

	__pgunmap(sq->pages.vaddr, len, sq->pages.vaddr_opaque);

	if (ctrl->dbbuf.doorbells) {
		__STORE_PTR(uint32_t *, sq->dbbuf.doorbell, 0);
		__STORE_PTR(uint32_t *, sq->dbbuf.eventidx, 0);
	}

	memset(sq, 0x0, sizeof(*sq));
}

static int nvme_configure_adminq(struct nvme_ctrl *ctrl, unsigned long sq_flags)
{
	int aqa;

	struct nvme_cq *cq = &ctrl->cq[NVME_AQ];
	struct nvme_sq *sq = &ctrl->sq[NVME_AQ];

	if (nvme_configure_cq(ctrl, NVME_AQ, NVME_AQ_QSIZE, 0)) {
		log_debug("failed to configure admin completion queue\n");
		return -1;
	}

	if (nvme_configure_sq(ctrl, NVME_AQ, NVME_AQ_QSIZE, cq, sq_flags)) {
		log_debug("failed to configure admin submission queue\n");
		goto discard_cq;
	}

	ctrl->adminq.cq = cq;
	ctrl->adminq.sq = sq;

	aqa = NVME_AQ_QSIZE - 1;
	aqa |= aqa << 16;

	mmio_write32(ctrl->regs, NVME_REG_AQA, cpu_to_le32(aqa));
	mmio_hl_write64(ctrl->regs, NVME_REG_ASQ, cpu_to_le64(sq->iova));
	mmio_hl_write64(ctrl->regs, NVME_REG_ACQ, cpu_to_le64(cq->iova));

	return 0;

discard_cq:
	nvme_discard_cq(ctrl, cq);
	return -1;
}

static int __admin(struct nvme_ctrl *ctrl, union nvme_cmd *sqe)
{
	return nvme_sync(ctrl, ctrl->adminq.sq, sqe, NULL, 0, NULL);
}

int nvme_create_iocq(struct nvme_ctrl *ctrl, int qid, int qsize, int vector)
{
	struct nvme_cq *cq = &ctrl->cq[qid];
	union nvme_cmd cmd;

	uint16_t qflags = NVME_Q_PC;
	uint16_t iv = 0;

	if (nvme_configure_cq(ctrl, qid, qsize, vector)) {
		log_debug("could not configure io completion queue\n");
		return -1;
	}

	if (vector != -1) {
		qflags |= NVME_CQ_IEN;
		iv = (uint16_t)vector;
	}

	cmd.create_cq = (struct nvme_cmd_create_cq) {
		.opcode = NVME_ADMIN_CREATE_CQ,
		.prp1   = cpu_to_le64(cq->iova),
		.qid    = cpu_to_le16((uint16_t)qid),
		.qsize  = cpu_to_le16((uint16_t)(qsize - 1)),
		.qflags = cpu_to_le16(qflags),
		.iv     = cpu_to_le16(iv),
	};

	return __admin(ctrl, &cmd);
}

int nvme_delete_iocq(struct nvme_ctrl *ctrl, int qid)
{
	union nvme_cmd cmd;

	nvme_discard_cq(ctrl, &ctrl->cq[qid]);

	cmd.delete_q = (struct nvme_cmd_delete_q) {
		.opcode = NVME_ADMIN_DELETE_CQ,
		.qid = cpu_to_le16((uint16_t)qid),
	};

	return __admin(ctrl, &cmd);
}

int nvme_create_iosq(struct nvme_ctrl *ctrl, int qid, int qsize, struct nvme_cq *cq,
		     unsigned long flags)
{
	struct nvme_sq *sq = &ctrl->sq[qid];
	union nvme_cmd cmd;

	if (nvme_configure_sq(ctrl, qid, qsize, cq, flags)) {
		log_debug("could not configure io submission queue\n");
		return -1;
	}

	cmd.create_sq = (struct nvme_cmd_create_sq) {
		.opcode = NVME_ADMIN_CREATE_SQ,
		.prp1   = cpu_to_le64(sq->iova),
		.qid    = cpu_to_le16((uint16_t)qid),
		.qsize  = cpu_to_le16((uint16_t)(qsize - 1)),
		.qflags = cpu_to_le16(NVME_Q_PC),
		.cqid   = cpu_to_le16((uint16_t)cq->id),
	};

	return __admin(ctrl, &cmd);
}

int nvme_delete_iosq(struct nvme_ctrl *ctrl, int qid)
{
	union nvme_cmd cmd;

	nvme_discard_sq(ctrl, &ctrl->sq[qid]);

	cmd.delete_q = (struct nvme_cmd_delete_q) {
		.opcode = NVME_ADMIN_DELETE_SQ,
		.qid = cpu_to_le16((uint16_t)qid),
	};

	return __admin(ctrl, &cmd);
}

int nvme_create_ioqpair(struct nvme_ctrl *ctrl, int qid, int qsize, int vector, unsigned long flags)
{
	if (nvme_create_iocq(ctrl, qid, qsize, vector)) {
		log_debug("could not create io completion queue\n");
		return -1;
	}

	if (nvme_create_iosq(ctrl, qid, qsize, &ctrl->cq[qid], flags)) {
		log_debug("could not create io submission queue\n");
		return -1;
	}

	return 0;
}

int nvme_delete_ioqpair(struct nvme_ctrl *ctrl, int qid)
{
	if (nvme_delete_iosq(ctrl, qid)) {
		log_debug("could not delete io submission queue\n");
		return -1;
	}

	if (nvme_delete_iocq(ctrl, qid)) {
		log_debug("could not delete io completion queue\n");
		return -1;
	}

	return 0;
}

#ifndef __APPLE__
#define _time_ns time_now().ts.tv_nsec
#else
#define _time_ns clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)
#endif

static int nvme_wait_rdy(struct nvme_ctrl *ctrl, unsigned short rdy)
{
	uint64_t cap;
	uint64_t timeout_ns;
	uint64_t deadline_ns;
	uint32_t csts;

	cap = le64_to_cpu(mmio_read64(ctrl->regs, NVME_REG_CAP));
	timeout_ns = 500 * (NVME_FIELD_GET(cap, CAP_TO) + 1) * 1000000;
	deadline_ns = _time_ns + timeout_ns;

	do {
		if ((uint64_t)_time_ns > deadline_ns) {
			log_debug("timed out\n");

			errno = ETIMEDOUT;
			return -1;
		}

		csts = le32_to_cpu(mmio_read32(ctrl->regs, NVME_REG_CSTS));
	} while (NVME_FIELD_GET(csts, CSTS_RDY) != rdy);

	return 0;
}

int nvme_enable(struct nvme_ctrl *ctrl)
{
	uint8_t css;
	uint32_t cc;
	uint64_t cap;

	cap = le64_to_cpu(mmio_read64(ctrl->regs, NVME_REG_CAP));
	css = NVME_FIELD_GET(cap, CAP_CSS);

	cc =
		NVME_FIELD_SET(ctrl->config.mps, CC_MPS) |
		NVME_FIELD_SET(NVME_CC_AMS_RR,	 CC_AMS) |
		NVME_FIELD_SET(NVME_CC_SHN_NONE, CC_SHN) |
		NVME_FIELD_SET(NVME_SQES,        CC_IOSQES) |
		NVME_FIELD_SET(NVME_CQES,        CC_IOCQES) |
		NVME_FIELD_SET(0x1,              CC_EN);

	if (css & NVME_CAP_CSS_CSI)
		cc |= NVME_FIELD_SET(NVME_CC_CSS_CSI, CC_CSS);
	else if (css & NVME_CAP_CSS_ADMIN)
		cc |= NVME_FIELD_SET(NVME_CC_CSS_ADMIN, CC_CSS);
	else
		cc |= NVME_FIELD_SET(NVME_CC_CSS_NVM, CC_CSS);

	mmio_write32(ctrl->regs, NVME_REG_CC, cpu_to_le32(cc));

	return nvme_wait_rdy(ctrl, 1);
}

int nvme_reset(struct nvme_ctrl *ctrl)
{
	uint32_t cc;

	cc = le32_to_cpu(mmio_read32(ctrl->regs, NVME_REG_CC));
	mmio_write32(ctrl->regs, NVME_REG_CC, cpu_to_le32(cc & 0xfe));

	return nvme_wait_rdy(ctrl, 0);
}

static int nvme_init_dbconfig(struct nvme_ctrl *ctrl)
{
	uint64_t prp1, prp2;
	union nvme_cmd cmd;
	void *opaque;

	if (__pgmap((void **)&ctrl->dbbuf.doorbells, __VFN_PAGESIZE, &opaque) < 0)
		return -1;

	if (_iommu_map_vaddr(__iommu_ctx(ctrl), ctrl->dbbuf.doorbells, __VFN_PAGESIZE, &prp1, 0x0,
		opaque))
		return -1;

	if (__pgmap((void **)&ctrl->dbbuf.eventidxs, __VFN_PAGESIZE, &opaque) < 0)
		return -1;

	if (_iommu_map_vaddr(__iommu_ctx(ctrl), ctrl->dbbuf.eventidxs, __VFN_PAGESIZE, &prp2, 0x0,
		opaque))
		return -1;

	cmd = (union nvme_cmd) {
		.opcode = NVME_ADMIN_DBCONFIG,
		.dptr.prp1 = cpu_to_le64(prp1),
		.dptr.prp2 = cpu_to_le64(prp2),
	};

	if (__admin(ctrl, &cmd))
		return -1;

	if (!(ctrl->opts.quirks & NVME_QUIRK_BROKEN_DBBUF)) {
		uint64_t cap;
		uint8_t dstrd;

		cap = le64_to_cpu(mmio_read64(ctrl->regs, NVME_REG_CAP));
		dstrd = NVME_FIELD_GET(cap, CAP_DSTRD);

		ctrl->adminq.cq->dbbuf.doorbell = cqhdbl(ctrl->dbbuf.doorbells, NVME_AQ, dstrd);
		ctrl->adminq.cq->dbbuf.eventidx = cqhdbl(ctrl->dbbuf.eventidxs, NVME_AQ, dstrd);

		ctrl->adminq.sq->dbbuf.doorbell = sqtdbl(ctrl->dbbuf.doorbells, NVME_AQ, dstrd);
		ctrl->adminq.sq->dbbuf.eventidx = sqtdbl(ctrl->dbbuf.eventidxs, NVME_AQ, dstrd);
	}

	return 0;
}

int nvme_init(struct nvme_ctrl *ctrl, const char *bdf, const struct nvme_ctrl_opts *opts)
{
	unsigned long long classcode;
	uint64_t cap;
	uint8_t mpsmin, mpsmax;
	uint16_t oacs;
	ssize_t len;
	void *opaque;
	void *vaddr;
	int ret;

	union nvme_cmd cmd = {};
	struct nvme_cqe cqe;

	if (opts)
		memcpy(&ctrl->opts, opts, sizeof(*opts));
	else
		memcpy(&ctrl->opts, &nvme_ctrl_opts_default, sizeof(*opts));

	if (pci_device_info_get_ull(bdf, "class", &classcode)) {
		log_debug("could not get device class code\n");
		return -1;
	}

	log_info("pci class code is 0x%06llx\n", classcode);

	if ((classcode & 0xffff00) != 0x010800) {
		log_debug("%s is not an NVMe device\n", bdf);
		errno = EINVAL;
		return -1;
	}

	if ((classcode & 0xff) == 0x03)
		ctrl->flags = NVME_CTRL_F_ADMINISTRATIVE;

	if (vfio_pci_open(&ctrl->pci, bdf))
		return -1;

	ctrl->regs = vfio_pci_map_bar(&ctrl->pci, 0, 0x1000, 0, PROT_READ | PROT_WRITE);
	if (!ctrl->regs) {
		log_debug("could not map controller registersn\n");
		return -1;
	}

	cap = le64_to_cpu(mmio_read64(ctrl->regs, NVME_REG_CAP));
	mpsmin = NVME_FIELD_GET(cap, CAP_MPSMIN);
	mpsmax = NVME_FIELD_GET(cap, CAP_MPSMAX);

	ctrl->config.mps = clamp_t(int, __VFN_PAGESHIFT - 12, mpsmin, mpsmax);

	if ((__mps_to_pageshift(ctrl->config.mps)) > __VFN_PAGESHIFT) {
		log_error("mpsmin too large\n");
		errno = EINVAL;
		return -1;
	} else if ((__mps_to_pageshift(ctrl->config.mps)) < __VFN_PAGESHIFT) {
		log_info("host memory page size is larger than mpsmax; clamping mps to %d\n",
			 ctrl->config.mps);
	}

	ctrl->config.mqes = NVME_FIELD_GET(cap, CAP_MQES);

	if (nvme_reset(ctrl)) {
		log_debug("could not reset controller\n");
		return -1;
	}

	/* map admin queue doorbells */
	ctrl->doorbells = vfio_pci_map_bar(&ctrl->pci, 0, 0x1000, 0x1000, PROT_WRITE);
	if (!ctrl->doorbells) {
		log_debug("could not map doorbells\n");
		return -1;
	}

	/* +2 because nsqr/ncqr are zero-based values and do not account for the admin queue */
	ctrl->sq = znew_t(struct nvme_sq, ctrl->opts.nsqr + 2);
	ctrl->cq = znew_t(struct nvme_cq, ctrl->opts.ncqr + 2);

	if (nvme_configure_adminq(ctrl, 0x0)) {
		log_debug("could not configure admin queue\n");
		return -1;
	}

	if (nvme_enable(ctrl)) {
		log_debug("could not enable controller\n");
		return -1;
	}

	if (ctrl->flags & NVME_CTRL_F_ADMINISTRATIVE)
		return 0;

	cmd = (union nvme_cmd) {
		.opcode = NVME_ADMIN_SET_FEATURES,
	};

	cmd.features.fid = NVME_FEAT_FID_NUM_QUEUES;
	cmd.features.cdw11 = cpu_to_le32(
		NVME_FIELD_SET(ctrl->opts.nsqr, FEAT_NRQS_NSQR) |
		NVME_FIELD_SET(ctrl->opts.ncqr, FEAT_NRQS_NCQR));

	if (nvme_admin(ctrl, &cmd, NULL, 0, &cqe)) {
		log_debug("could not set number of queues\n");
		return -1;
	}

	ctrl->config.nsqa = min_t(int, ctrl->opts.nsqr,
				  NVME_FIELD_GET(le32_to_cpu(cqe.dw0), FEAT_NRQS_NSQR));
	ctrl->config.ncqa = min_t(int, ctrl->opts.ncqr,
				  NVME_FIELD_GET(le32_to_cpu(cqe.dw0), FEAT_NRQS_NCQR));

	len = __pgmap(&vaddr, NVME_IDENTIFY_DATA_SIZE, &opaque);
	if (len < 0)
		return -1;

	if (_iommu_map_vaddr(__iommu_ctx(ctrl), vaddr, len, NULL, IOMMU_MAP_EPHEMERAL, opaque)) {
		log_error("failed to map vaddr\n");
		return -1;
	}

	cmd.identify = (struct nvme_cmd_identify) {
		.opcode = NVME_ADMIN_IDENTIFY,
		.cns = NVME_IDENTIFY_CNS_CTRL,
	};

	ret = nvme_admin(ctrl, &cmd, vaddr, len, NULL);
	if (ret) {
		log_debug("could not identify\n");
		goto out;
	}

	memcpy(ctrl->serial, ((char *) vaddr) +
		NVME_IDENTIFY_CTRL_SERIAL_NUMBER, sizeof(ctrl->serial));
	oacs = le16_to_cpu(*(leint16_t *)(((uintptr_t) vaddr) + NVME_IDENTIFY_CTRL_OACS));

	if (oacs & NVME_IDENTIFY_CTRL_OACS_DBCONFIG)
		ret = nvme_init_dbconfig(ctrl);

out:
	log_fatal_if(iommu_unmap_vaddr(__iommu_ctx(ctrl), vaddr, NULL), "iommu_unmap_vaddr\n");
	__pgunmap(vaddr, len, opaque);

	return ret;
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

	//vfio_close(ctrl->pci.vfio);
}
