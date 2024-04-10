/* SPDX-License-Identifier: LGPL-2.1-or-later or MIT */

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2022 The libvfn Authors. All Rights Reserved.
 *
 * This library (libvfn) is dual licensed under the GNU Lesser General
 * Public License version 2.1 or later or the MIT license. See the
 * COPYING and LICENSE files for more information.
 */

#ifndef LIBVFN_SKIPLIST_H
#define LIBVFN_SKIPLIST_H

#include "ccan/list/list.h"

#ifndef SKIPLIST_LEVELS
#define SKIPLIST_LEVELS 8
#endif

#define skiplist_top(h, t, k) \
	list_top(&(h)->heads[k], t, list[k])

#define skiplist_next(h, n, k) \
	list_next(&(h)->heads[k], (n), list[k])

#define skiplist_add_after(h, p, n, k) \
	list_add_after(&(h)->heads[k], &(p)->list[k], &(n)->list[k])

#define skiplist_add(h, n, k) \
	list_add(&(h)->heads[k], &(n)->list[k])

#define skiplist_del_from(h, n, k) \
	list_del_from(&(h)->heads[k], &(n)->list[k])

#define skiplist_for_each_safe(h, n, next, k) \
	list_for_each_safe(&(h)->heads[k], n, next, list[k])

struct skiplist_node {
	struct list_node list[SKIPLIST_LEVELS];
};

#define skiplist_entry(ptr, type, member) container_of(ptr, type, member)

struct skiplist {
	int height;
	struct skiplist_node sentinel;
	struct list_head heads[SKIPLIST_LEVELS];
};

typedef void (*skiplist_iter_fn)(void *opaque, struct skiplist_node *n);


void skiplist_init(struct skiplist *list);
void skiplist_clear_with(struct skiplist *list, skiplist_iter_fn fn, void *opaque);
struct skiplist_node *skiplist_find(struct skiplist *list, const void *key,
				    int (*cmp)(const void *key, const struct skiplist_node *n),
				    struct skiplist_node **path);
void skiplist_link(struct skiplist *list, struct skiplist_node *n,
		   struct skiplist_node *update[SKIPLIST_LEVELS]);
void skiplist_erase(struct skiplist *list, struct skiplist_node *n,
		    struct skiplist_node *update[SKIPLIST_LEVELS]);

#endif /* LIBVFN_SKIPLIST_H */
