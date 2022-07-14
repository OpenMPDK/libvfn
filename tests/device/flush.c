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

#include "ccan/opt/opt.h"
#include "ccan/tap/tap.h"

#include "vfn/nvme.h"

#include "common.h"

static int test_flush(uint32_t nsid)
{
	union nvme_cmd cmd;

	cmd = (union nvme_cmd) {
		.opcode = nvme_cmd_flush,
		.nsid = cpu_to_le32(nsid),
	};

	return nvme_oneshot(&ctrl, sq, &cmd, NULL, 0x0, NULL);
}

int main(int argc, char **argv)
{
	setup_io(argc, argv);

	plan_tests(2);

	if (!nsid) {
		skip(1, "namespace identifier not set");
		goto out;
	}

	ok(test_flush(nsid) == 0, "flush one");

out:
	ok(test_flush(NVME_NSID_ALL) == 0, "flush all");

	teardown();

	return 0;
}
