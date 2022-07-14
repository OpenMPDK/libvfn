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

static int test_io(void)
{
	void *vaddr;
	ssize_t len;
	int ret;

	union nvme_cmd cmd;

	len = pgmap(&vaddr, __VFN_PAGESIZE);
	if (len < 0)
		err(1, "failed to map memory");

	cmd.rw = (struct nvme_cmd_rw) {
		.opcode = nvme_cmd_read,
		.nsid = cpu_to_le32(nsid),
	};

	ret = nvme_oneshot(&ctrl, sq, &cmd, vaddr, len, NULL);

	pgunmap(vaddr, len);

	return ret;
}

int main(int argc, char **argv)
{
	setup_io(argc, argv);

	plan_tests(1);

	if (!nsid) {
		skip(1, "namespace identifier not set");
		goto out;
	}

	ok(test_io() == 0, "io read");

out:
	teardown();

	return 0;
}
