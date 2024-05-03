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

#include <vfn/nvme.h>
#include "iommu/context.h"
#include "types.h"
#include "crc64table.h"

uint64_t nvme_crc64(uint64_t crc, const unsigned char *buffer, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		crc = (crc >> 8) ^ crc64_nvme_table[(crc & 0xff) ^ buffer[i]];

	return crc ^ (uint64_t)~0;
}

int nvme_set_errno_from_cqe(struct nvme_cqe *cqe)
{
	errno = le16_to_cpu(cqe->sfp) >> 1 ? EIO : 0;

	return errno ? -1 : 0;
}

int nvme_aer(struct nvme_ctrl *ctrl, void *opaque)
{
	struct nvme_rq *rq;
	union nvme_cmd cmd = { .opcode = NVME_ADMIN_ASYNC_EVENT };

	rq = nvme_rq_acquire_atomic(ctrl->adminq.sq);
	if (!rq) {
		errno = EBUSY;
		return -1;
	}

	cmd.cid = rq->cid | NVME_CID_AER;
	rq->opaque = opaque;

	/* rq_exec overwrites the command identifier, so use sq_exec */
	nvme_sq_exec(ctrl->adminq.sq, &cmd);

	return 0;
}

int nvme_sync(struct nvme_ctrl *ctrl, struct nvme_sq *sq, union nvme_cmd *sqe, void *buf,
	      size_t len, struct nvme_cqe *cqe_copy)
{
	struct nvme_cqe cqe;
	struct nvme_rq *rq;
	uint64_t iova;
	bool do_unmap = false;
	int ret = 0;

	void *opaque = NULL; // Could be null, as we're always mapped?
	if (buf) {
		struct iommu_ctx *ctx = __iommu_ctx(ctrl);

		if (!iommu_translate_vaddr(ctx, buf, &iova)) {
			do_unmap = true;

			if (_iommu_map_vaddr(ctx, buf, len, &iova, IOMMU_MAP_EPHEMERAL, opaque)) {
				log_debug("failed to map vaddr\n");
				return -1;
			}
		}
	}

	rq = nvme_rq_acquire_atomic(sq);
	if (!rq)
		return -1;

	if (buf) {
		ret = nvme_rq_map_prp(ctrl, rq, sqe, iova, len);
		if (ret) {
			goto release_rq;
		}
	}

	nvme_rq_exec(rq, sqe);

	while (nvme_rq_spin(rq, &cqe) < 0) {
		if (errno == EAGAIN) {
			log_error("SPURIOUS CQE (cq %" PRIu16 " cid %" PRIu16 ")\n",
				  (uint16_t)rq->sq->cq->id, cqe.cid);

			continue;
		}

		ret = -1;

		break;
	}

	if (cqe_copy)
		memcpy(cqe_copy, &cqe, 1 << NVME_CQES);

release_rq:
	nvme_rq_release_atomic(rq);

	if (buf && do_unmap)
		log_fatal_if(iommu_unmap_vaddr(__iommu_ctx(ctrl), buf, NULL),
			     "iommu_unmap_vaddr\n");

	return ret;
}

int nvme_admin(struct nvme_ctrl *ctrl, union nvme_cmd *sqe, void *buf, size_t len,
	       struct nvme_cqe *cqe_copy)
{
	return nvme_sync(ctrl, ctrl->adminq.sq, sqe, buf, len, cqe_copy);
}
