// SPDX-License-Identifier: BSD-3-Clause

/*
 * Copyright 2020 Mellanox Technologies, Ltd
 * Copyright (C) 2022 The libvfn Authors. All Rights Reserved.
 *
 * From DPDK; modified by the libvfn Authors.
 */

#define log_fmt(fmt) "support/ticks: " fmt

#include "vfn/support.h"

#ifndef __APPLE__
#include "ccan/time/time.h"
#endif

#define TICKS_PER_10MHZ 10000000ULL

uint64_t __vfn_ticks_freq;

#ifndef __APPLE__
static uint64_t measure_ticks_freq(void)
{
	struct timemono t_start, t_end;
	struct timerel t_sleep, t_diff;
	uint64_t hz, end, start, ns;
	double secs;

	log_debug("measuring tick frequency\n");

	t_sleep = time_from_nsec(NS_PER_SEC / 10);

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &t_start.ts))
		return 0;

	start = get_ticks();

	nanosleep(&t_sleep.ts, NULL);

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &t_end.ts))
		return 0;

	end = get_ticks();

	t_diff = timemono_between(t_end, t_start);
	ns = time_to_nsec(t_diff);

	secs = (double)ns/NS_PER_SEC;
	hz = (uint64_t)((double)(end - start)/secs);

	return ROUND(hz, TICKS_PER_10MHZ);
}

static uint64_t estimate_ticks_freq(void)
{
	uint64_t start;

	log_debug("warning: estimating tick frequency; clock timings may be inaccurate\n");

	start = get_ticks();
	__usleep(1E6);

	return ROUND(get_ticks() - start, TICKS_PER_10MHZ);
}
#endif

static void __attribute__((constructor)) init_ticks_freq(void)
{
	uint64_t freq;

	freq = get_ticks_freq_arch();
	if (!freq)
	#ifndef __APPLE__
		freq = measure_ticks_freq();

	if (!freq)
		freq = estimate_ticks_freq();

	#else
		abort();
	#endif
	log_debug("tick frequency is ~%" PRIu64 " Hz\n", freq);

	__vfn_ticks_freq = freq;
}
