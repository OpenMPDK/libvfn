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

#include <vfn/nvme.h>

#include "ccan/err/err.h"
#include "ccan/opt/opt.h"
#include "ccan/tap/tap.h"

#include "common.h"

struct nvme_ctrl ctrl;
struct nvme_sq *sq;
struct nvme_cq *cq;

char *bdf;
unsigned long nsid;
bool show_usage;

struct opt_table opts[] = {
	OPT_WITHOUT_ARG("-h|--help", opt_set_bool, &show_usage, "show usage"),
	OPT_WITH_ARG("-d|--device BDF", opt_set_charp, opt_show_charp, &bdf, "pci device"),
	OPT_WITH_ARG("-N|--nsid NSID", opt_set_ulongval, opt_show_ulongval, &nsid,
		     "namespace identifier"),
	OPT_ENDTABLE,
};

void parse_args(int argc, char *argv[])
{
	opt_register_table(opts, NULL);
	opt_parse(&argc, argv, opt_log_stderr_exit);

	if (show_usage)
		opt_usage_and_exit(NULL);

	opt_free_table();
}

void setup(int argc, char *argv[])
{
	parse_args(argc, argv);

	if (!bdf) {
		plan_skip_all("no test device");
		exit(EXIT_SKIPPED);
	}

	if (nvme_init(&ctrl, bdf, NULL))
		err(1, "failed to init nvme controller");
}

void setup_io(int argc, char *argv[])
{
	setup(argc, argv);

	if (nvme_create_ioqpair(&ctrl, 1, 8, 0x0))
		err(1, "could not create i/o queue pair");

	sq = &ctrl.sq[1];
	cq = &ctrl.cq[1];
}

void teardown(void)
{
	if (sq && nvme_delete_ioqpair(&ctrl, 1))
		err(1, "could not delete io queue pair");

	nvme_close(&ctrl);
}
