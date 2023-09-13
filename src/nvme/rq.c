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

#define log_fmt(fmt) "nvme/rq: " fmt

#include <vfn/nvme.h>
#include "ccan/minmax/minmax.h"


static inline int __map_first(leint64_t *prp1, leint64_t *prplist, uint64_t iova, size_t len,
			      int pageshift)
{
	size_t pagesize = 1 << pageshift;
	int max_prps = 1 << (pageshift - 3);

	/* number of prps required to map the buffer */
	int prpcount = 1;

	*prp1 = cpu_to_le64(iova);

	/* account for what is covered with the first prp */
	len -= min_t(size_t, len, pagesize - (iova & (pagesize - 1)));

	/* any residual just adds more prps */
	if (len)
		prpcount += (int)ALIGN_UP(len, pagesize) >> pageshift;

	if (prpcount > 1 && !ALIGNED(iova, pagesize))
		/* align down to simplify loop below */
		iova = ALIGN_DOWN(iova, pagesize);

	if (prpcount > max_prps) {
		errno = EINVAL;
		return 0;
	}

	/*
	 * Map the remaining parts of the buffer into prp2/prplist. iova will be
	 * aligned from the above, which simplifies this.
	 */
	for (int i = 1; i < prpcount; i++)
		prplist[i - 1] = cpu_to_le64(iova + ((uint64_t)i << pageshift));

	/*
	 * prpcount may be zero if the buffer length was less than the page
	 * size, so clamp it to 1 in that case.
	 */
	return clamp_t(int, prpcount, 1, prpcount);
}

static inline int __map_aligned(leint64_t *prplist, int prpcount, uint64_t iova, int pageshift)
{
	size_t pagesize = 1 << pageshift;

	/*
	 * __map_aligned is used exclusively for mapping into the prplist
	 * entries where addresses must be page size aligned.
	 */
	assert(ALIGNED(iova, pagesize));

	for (int i = 0; i < prpcount; i++)
		prplist[i] = cpu_to_le64(iova + ((uint64_t)i << pageshift));

	return prpcount;
}

int nvme_rq_map_prp(struct nvme_ctrl *ctrl, struct nvme_rq *rq, union nvme_cmd *cmd, uint64_t iova,
		    size_t len)
{
	int prpcount;
	leint64_t *prplist = (leint64_t *)rq->page.vaddr;

	prpcount = __map_first(&cmd->dptr.prp1, prplist, iova, len,
			       __mps_to_pageshift(ctrl->config.mps));
	if (!prpcount) {
		errno = EINVAL;
		return -1;
	}

	if (prpcount == 2)
		cmd->dptr.prp2 = prplist[0];
	else if (prpcount > 2)
		cmd->dptr.prp2 = cpu_to_le64(rq->page.iova);
	else
		cmd->dptr.prp2 = 0x0;

	return 0;
}

#ifndef __APPLE__
int nvme_rq_mapv_prp(struct nvme_ctrl *ctrl, struct nvme_rq *rq, union nvme_cmd *cmd,
		     struct iovec *iov, int niov)
{
	int prpcount, _prpcount;
	leint64_t *prplist = (leint64_t *)rq->page.vaddr;
	uint64_t iova = (uint64_t)iov->iov_base;
	size_t len = iov->iov_len;
	int pageshift = __mps_to_pageshift(ctrl->config.mps);
	size_t pagesize = 1 << pageshift;
	int max_prps = 1 << (pageshift - 3);

	/* map the first segment */
	prpcount = __map_first(&cmd->dptr.prp1, prplist, iova, len, pageshift);

	/*
	 * At this point, one of three conditions must hold:
	 *
	 *   a) a single prp entry was set up by __map_first, or
	 *   b) the iovec only has a single entry, or
	 *   c) the first buffer ends on a page size boundary
	 *
	 * If none holds, the buffer(s) within the iovec cannot be mapped given
	 * the PRP alignment requirements.
	 */
	if (!(prpcount == 1 || niov == 1 || ALIGNED(iova + len, pagesize))) {
		log_error("iov[0].iov_base/len invalid\n");

		goto invalid;
	}

	/* map remaining iovec entries; these must be page size aligned */
	for (int i = 1; i < niov; i++) {
		iova = (uint64_t)iov[i].iov_base;
		len = iov[i].iov_len;

		_prpcount = max_t(int, 1, (int)len >> pageshift);

		if (prpcount + _prpcount > max_prps) {
			log_error("too many prps required\n");

			goto invalid;
		}


		if (!ALIGNED(iova, pagesize)) {
			log_error("unaligned iov[%u].iov_base (0x%" PRIx64 ")\n", i, iova);

			goto invalid;
		}

		/* all entries but the last must have a page size aligned len */
		if (i < niov - 1 && !ALIGNED(len, pagesize)) {
			log_error("unaligned iov[%u].len (%zu)\n", i, len);

			goto invalid;
		}

		prpcount += __map_aligned(&prplist[prpcount - 1], _prpcount, iova, pageshift);
	}

	if (prpcount == 2)
		cmd->dptr.prp2 = prplist[0];
	else if (prpcount > 2)
		cmd->dptr.prp2 = cpu_to_le64(rq->page.iova);
	else
		cmd->dptr.prp2 = 0x0;

	return 0;

invalid:
	errno = EINVAL;
	return -1;
}
#endif

int nvme_rq_spin(struct nvme_rq *rq, struct nvme_cqe *cqe_copy)
{
	struct nvme_cq *cq = rq->sq->cq;
	struct nvme_cqe cqe;

	nvme_cq_get_cqes(cq, &cqe, 1);
	nvme_cq_update_head(cq);

	if (cqe_copy)
		memcpy(cqe_copy, &cqe, sizeof(*cqe_copy));

	if (cqe.cid != rq->cid) {
		errno = EAGAIN;
		return -1;
	}

	if (!nvme_cqe_ok(&cqe)) {
		#ifdef __APPLE__
		if (false) {
		#else
		if (logv(LOG_DEBUG)) {
		#endif
			uint16_t status = le16_to_cpu(cqe.sfp) >> 1;

			log_debug("cqe status 0x%" PRIx16 "\n", (uint16_t)(status & 0x7ff));
		}

		return nvme_set_errno_from_cqe(&cqe);
	}

	return 0;
}
