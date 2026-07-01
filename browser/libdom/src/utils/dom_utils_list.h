/*
 * This file is part of libdom.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2009 Bo Yang <struggleyb.nku@gmail.com>
 *
 * This file contains the list structure used to compose lists.
 *
 * Note: This is a implementation of a doubld-linked cyclar list.
 *
 * MacSurf: static inline bodies extracted to libdom_c89_helpers.c
 * for CW8 C89 compatibility.
 */

#ifndef dom_utils_list_h_
#define dom_utils_list_h_

#include <stddef.h>

struct list_entry {
	struct list_entry *prev;
	struct list_entry *next;
};

extern void list_init(struct list_entry *ent);
extern void list_append(struct list_entry *head, struct list_entry *new_entry);
extern void list_del(struct list_entry *ent);

#endif
