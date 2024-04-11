// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2022 The libvfn Authors. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <stdbool.h>
#include <stdint.h>

#include <nvme/types.h>

#include "ccan/array_size/array_size.h"
#include "ccan/err/err.h"
#include "ccan/opt/opt.h"
#include "ccan/tap/tap.h"

#include "vfn/nvme.h"

#include "common.h"

#define __foreach(x, arr) \
	for (typeof(arr[0]) *x = arr; x < &arr[ARRAY_SIZE(arr)]; x++)

static bool aen_received;

static int get_smart_log(void)
{
	struct nvme_smart_log *log = NULL;
	union nvme_cmd cmd;
	ssize_t len;
	int ret = 0;

	len = pgmap((void **)&log, sizeof(*log));
	if (len < 0)
		err(1, "failed to map memory");

	cmd.log = (struct nvme_cmd_log) {
		.opcode = nvme_admin_get_log_page,
		.numdl = cpu_to_le16((len >> 2) - 1),
		.nsid = cpu_to_le32(NVME_NSID_ALL),
		.lid = NVME_LOG_LID_SMART,
	};

	ret = nvme_admin(&ctrl, &cmd, log, len, NULL);

	pgunmap(log, len);

	return ret;
}

static void handle_aen(struct nvme_cqe *cqe)
{
	uint32_t dw0 = le32_to_cpu(cqe->dw0);
	int type, info, lid;

	uint32_t expected =
		NVME_AER_SMART |
		NVME_AER_SMART_TEMPERATURE_THRESHOLD << 8 |
		NVME_LOG_LID_SMART << 16;

	type = NVME_AEN_TYPE(dw0);
	info = NVME_AEN_INFO(dw0);
	lid = NVME_AEN_LID(dw0);

	diag("got aen 0x%"PRIx32" (type 0x%x info 0x%x lid 0x%x)", dw0, type, info, lid);

	if (dw0 == expected)
		aen_received = true;
}

static int test_aer(void)
{
	union nvme_cmd cmd;
	struct nvme_rq *rq;
	struct nvme_cqe cqe, cqes[2];
	uint64_t timeout_ns = 1000000000;
	int ret;

	uint32_t temp_thresh;

	cmd.features = (struct nvme_cmd_features) {
		.opcode = nvme_admin_set_features,
		.fid = NVME_FEAT_FID_ASYNC_EVENT,
		.cdw11 = cpu_to_le32(NVME_SET(NVME_SMART_CRIT_TEMPERATURE, FEAT_AE_SMART)),
	};

	if (nvme_admin(&ctrl, &cmd, NULL, 0, NULL))
		err(1, "could not set asynchronous event configuration");

	cmd.features = (struct nvme_cmd_features) {
		.opcode = nvme_admin_get_features,
		.fid = NVME_FEAT_FID_TEMP_THRESH,
	};

	if (nvme_admin(&ctrl, &cmd, NULL, 0, &cqe))
		err(1, "could not get current temperature threshold");

	temp_thresh = le32_to_cpu(cqe.dw0);

	diag("current temperature threshold is %"PRIu32" K", temp_thresh);

	if (nvme_aer(&ctrl, NULL))
		err(1, "could not post aer");

	cmd.features = (struct nvme_cmd_features) {
		.opcode = nvme_admin_set_features,
		.fid = NVME_FEAT_FID_TEMP_THRESH,
		.cdw11 = cpu_to_le32(NVME_SET(200, FEAT_TT_TMPTH)),
	};

	/*
	 * We cannot use on the synchronous "oneshot" admin queue helper since
	 * there is no guarantee that the Set Feature command completes prior to
	 * the AER being delivered. So drop down a level and handle submission
	 * and completions on our own.
	 */

	rq = nvme_rq_acquire(ctrl.adminq.sq);
	if (!rq)
		err(1, "could not acquire rq");

	nvme_rq_exec(rq, &cmd);

	ret = nvme_cq_wait_cqes(ctrl.adminq.cq, cqes, 2, timeout_ns);
	if (ret < 2 && errno == ETIMEDOUT)
		err(errno, "not enough completions in time");

	__foreach(_cqe, cqes) {
		if (_cqe->cid & NVME_CID_AER) {
			/* clear the AER bit so nvme_rq_from_cqe() can find the rq */
			_cqe->cid &= ~NVME_CID_AER;

			handle_aen(_cqe);
		}

		nvme_rq_release(nvme_rq_from_cqe(&ctrl, _cqe));
	}

	assert(aen_received);

	cmd.features = (struct nvme_cmd_features) {
		.opcode = nvme_admin_set_features,
		.fid = NVME_FEAT_FID_TEMP_THRESH,
		.cdw11 = cpu_to_le32(NVME_SET(temp_thresh, FEAT_TT_TMPTH)),
	};

	if (nvme_admin(&ctrl, &cmd, NULL, 0, &cqe))
		err(1, "could not reset temperature threshold");

	if (get_smart_log())
		err(1, "could not clear event");

	return 0;
}

int main(int argc, char **argv)
{
	setup(argc, argv);

	plan_tests(1);

	ok(test_aer() == 0, "aer");

	teardown();

	return 0;
}
