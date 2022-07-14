/* SPDX-License-Identifier: GPL-2.0-or-later */

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

#ifndef LIBVFN_TESTS_DEVICE_COMMON_H
#define LIBVFN_TESTS_DEVICE_COMMON_H

#define EXIT_SKIPPED 77

extern struct nvme_ctrl ctrl;
extern struct nvme_sq *sq;
extern struct nvme_cq *cq;

extern char *bdf;
extern bool show_usage;
extern unsigned long nsid;

extern struct opt_table opts[];

void parse_args(int argc, char *argv[]);
void setup(int argc, char *argv[]);
void setup_io(int argc, char *argv[]);
void teardown(void);

#endif /* LIBVFN_TESTS_DEVICE_COMMON_H */
