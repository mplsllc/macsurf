/*
 * retro68_malloc.c — MacSurf allocator for Retro68 (OS X 10.3 Carbon CFM).
 *
 * Based on Retro68 libretro/malloc.c by Wolfgang Thaller, relicensed under
 * the same GPL+RuntimeException terms.
 *
 * Uses ONLY NewPtr / DisposePtr from the Toolbox Memory Manager.
 * SetPtrSize and GetPtrSize are avoided — on OS X 10.3 Carbon CFM they
 * can resolve through NULL routine descriptors.
 *
 * Each allocation carries a hidden size_t prefix so realloc/free know the
 * block size without calling GetPtrSize.  NewPtr on PowerPC returns 16-byte
 * aligned memory, so payload alignment is fine for all standard types.
 */

#include <stdlib.h>
#include <errno.h>
#include <reent.h>
#include <string.h>
#include <MacMemory.h>

void referenceMyMalloc(void) {}

/* ---------- internal helpers ---------- */

static inline void *header_to_payload(void *hdr)
{
	return (void *)((size_t *)hdr + 1);
}

static inline void *payload_to_header(void *payload)
{
	return (void *)((size_t *)payload - 1);
}

/* ---------- newlib _r entry points ---------- */

void *_malloc_r(struct _reent *reent_ptr, size_t sz)
{
	size_t *p = (size_t *)NewPtr(sz + sizeof(size_t));

	if (!p) {
		reent_ptr->_errno = ENOMEM;
		return NULL;
	}
	*p = sz;
	return header_to_payload(p);
}

void *_calloc_r(struct _reent *reent_ptr, size_t count, size_t sz)
{
	size_t total = count * sz;
	void *p = _malloc_r(reent_ptr, total);
	if (p)
		memset(p, 0, total);
	return p;
}

void _free_r(struct _reent *reent_ptr, void *ptr)
{
	(void)reent_ptr;
	if (ptr)
		DisposePtr(payload_to_header(ptr));
}

void *_realloc_r(struct _reent *reent_ptr, void *ptr, size_t sz)
{
	if (!ptr)
		return _malloc_r(reent_ptr, sz);
	if (sz == 0) {
		_free_r(reent_ptr, ptr);
		return NULL;
	}

	size_t *old_hdr = (size_t *)payload_to_header(ptr);
	size_t old_sz = *old_hdr;
	void *new_p = _malloc_r(reent_ptr, sz);
	if (!new_p)
		return NULL;
	memcpy(new_p, ptr, sz < old_sz ? sz : old_sz);
	_free_r(reent_ptr, ptr);
	return new_p;
}

/* ---------- standard C names ---------- */

void *malloc(size_t sz)           { return _malloc_r(_REENT, sz); }
void  free(void *p)               { _free_r(_REENT, p); }
void *realloc(void *ptr, size_t sz) { return _realloc_r(_REENT, ptr, sz); }
void *calloc(size_t n, size_t sz) { return _calloc_r(_REENT, n, sz); }

void *memalign(size_t alignment, size_t sz)
{
	(void)alignment;
	/* NewPtr on PowerPC returns 16-byte aligned blocks, sufficient
	 * for all standard types.  If stricter alignment is ever needed
	 * this will need an overallocate-and-align strategy. */
	return _malloc_r(_REENT, sz);
}
