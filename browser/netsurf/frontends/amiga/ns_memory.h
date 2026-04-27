/*
 * Copyright 2016 Chris Young <chris@unsatisfactorysoftware.co.uk>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef AMIGA_MEMORY_H
#define AMIGA_MEMORY_H

#include <exec/types.h>

/* Alloc/free chip memory */
#ifdef __amigaos4__
#define ami_memory_chip_alloc(s) malloc(s)
#define ami_memory_chip_free(p) free(p)
#else
#define ami_memory_chip_alloc(s) AllocVec(s, MEMF_CHIP)
#define ami_memory_chip_free(p) FreeVec(p)
#endif

/* Alloc/free a block cleared to non-zero */
#ifdef __amigaos4__
#define ami_memory_clear_alloc(s,v) AllocVecTags(s, AVT_ClearWithValue, v, TAG_DONE)
#define ami_memory_clear_free(p) FreeVec(p)
#else
void *ami_memory_clear_alloc(size_t size, UBYTE value);
#define ami_memory_clear_free(p) free(p)
#endif

/* Itempool cross-compatibility */
#ifdef __amigaos4__
#define ami_memory_itempool_create(s) AllocSysObjectTags(ASOT_ITEMPOOL, \
		ASOITEM_MFlags, MEMF_PRIVATE, \
		ASOITEM_ItemSize, s, \
		ASOITEM_GCPolicy, ITEMGC_AFTERCOUNT, \
		ASOITEM_GCParameter, 100, \
		TAG_DONE)
#define ami_memory_itempool_delete(p) FreeSysObject(ASOT_ITEMPOOL, p)
#define ami_memory_itempool_alloc(p,s) ItemPoolAlloc(p)
#define ami_memory_itempool_free(p,i,s) ItemPoolFree(p,i)
#else
#define ami_memory_itempool_create(s) ((APTR)1)
#define ami_memory_itempool_delete(p) ((void)0)
#define ami_memory_itempool_alloc(p,s) malloc(s)
#define ami_memory_itempool_free(p,i,s) free(i)
#endif

/* clib2 slab allocator */
#ifndef __amigaos4__
void ami_memory_slab_dump(BPTR fh);
struct Interrupt *ami_memory_init(void);
void ami_memory_fini(struct Interrupt *memhandler);
#endif

#endif //AMIGA_MEMORY_H

