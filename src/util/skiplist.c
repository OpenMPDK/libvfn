// SPDX-License-Identifier: LGPL-2.1-or-later or MIT

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2023 The libvfn Authors. All Rights Reserved.
 *
 * This library (libvfn) is dual licensed under the GNU Lesser General
 * Public License version 2.1 or later or the MIT license. See the
 * COPYING and LICENSE files for more information.
 */

#include <stdlib.h>

#ifndef __APPLE__
#define _rand rand
#else
#define _rand arc4random
#endif

#include "skiplist.h"

void skiplist_init(struct skiplist *list)
{
	list->height = 0;

	for (int k = 0; k < SKIPLIST_LEVELS; k++) {
		list_head_init(&list->heads[k]);
		skiplist_add(list, &list->sentinel, k);
	}
}

void skiplist_clear_with(struct skiplist *list, skiplist_iter_fn fn, void *opaque)
{
	struct skiplist_node *n, *next;
	int k = list->height;

	do {
		skiplist_for_each_safe(list, n, next, k) {
			if (n == NULL || n == &list->sentinel)
				continue;

			skiplist_del_from(list, n, k);

			if (k == 0 && fn)
				fn(opaque, n);
		}
	} while (--k >= 0);

	list->height = 0;
}

struct skiplist_node *skiplist_find(struct skiplist *list, const void *key,
				    int (*cmp)(const void *key, const struct skiplist_node *n),
				    struct skiplist_node **path)
{
	struct skiplist_node *next, *p = &list->sentinel;
	int k = list->height;

	do {
		next = skiplist_next(list, p, k);

		/* advance at this level as long as key is larger than next */
		while (next && cmp(key, next) > 0) {
			p = next;
			next = skiplist_next(list, p, k);
		}

		/*
		 * Our key is smaller than next or we reached the end of the
		 * list.
		 *
		 * Drop down a level, and record the node where we did so.
		 */
		if (path)
			path[k] = p;
	} while (--k >= 0);

	/* at this point we are at level zero; check if we have a winner */
	if (next && cmp(key, next) == 0)
		return next;

	return NULL;
}

static inline int __skiplist_random_level(void)
{
	int k = 0;

	while (k < SKIPLIST_LEVELS - 1 && (_rand() > (RAND_MAX / 2)))
		k++;

	return k;
}

#define SKIPLIST_RANDOM_LEVEL __skiplist_random_level()

void skiplist_link(struct skiplist *list, struct skiplist_node *n,
		   struct skiplist_node *update[SKIPLIST_LEVELS])
{
	int k = SKIPLIST_RANDOM_LEVEL;

	if (k > list->height) {
		/* increase the height of the skiplist */
		k = ++(list->height);

		/* new level k; insert new node after the sentinel */
		update[k] = &list->sentinel;
	}

	do {
		skiplist_add_after(list, update[k], n, k);
	} while (--k >= 0);
}

void skiplist_erase(struct skiplist *list, struct skiplist_node *n,
		    struct skiplist_node *update[SKIPLIST_LEVELS])
{
	for (int k = 0; k <= list->height; k++) {
		struct skiplist_node *next = skiplist_next(list, update[k], k);

		if (next != n)
			break;

		skiplist_del_from(list, n, k);
	}

	/* reduce height if possible */
	while (list->height && skiplist_next(list, &list->sentinel, list->height) == NULL)
		list->height--;
}
