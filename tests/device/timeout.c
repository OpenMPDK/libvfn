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

#include "ccan/err/err.h"
#include "ccan/opt/opt.h"
#include "ccan/tap/tap.h"

#include "vfn/nvme.h"

#include "common.h"

static int test_timeout(void)
{
	struct nvme_cqe cqe;
	uint64_t timeout_ns = 1000000000;
	int ret;

	if (nvme_aer(&ctrl, NULL))
		err(1, "could not enable aen");

	ret = nvme_cq_wait_cqes(ctrl.adminq.cq, &cqe, 1, timeout_ns);
	if (ret != 1 || errno != ETIMEDOUT)
		return -1;

	return 0;
}

int main(int argc, char **argv)
{
	setup(argc, argv);

	plan_tests(1);

	ok(test_timeout() == 0, "timeout");

	teardown();

	return 0;
}
