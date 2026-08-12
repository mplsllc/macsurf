/*
 * MacSurf wrapper - dom/bindings/hubbub/errors.h
 * Self-contained: inlines both the DOM binding error enum AND the
 * HUBBUB_* constants it references, so no dependency on the libhubbub
 * errors.h (which CW8 can't reliably find via the access paths).
 */

#ifndef dom_hubbub_errors_h_
#define dom_hubbub_errors_h_

/* Inlined HUBBUB error values (mirror of libhubbub/include/hubbub/errors.h)
 * Values must stay in sync with the real hubbub enum. */
#ifndef hubbub_errors_h_
#define hubbub_errors_h_
typedef enum hubbub_error {
	HUBBUB_OK               = 0,
	HUBBUB_REPROCESS        = 1,
	HUBBUB_ENCODINGCHANGE   = 2,
	HUBBUB_PAUSED           = 3,
	HUBBUB_NOMEM            = 5,
	HUBBUB_BADPARM          = 6,
	HUBBUB_INVALID          = 7,
	HUBBUB_FILENOTFOUND     = 8,
	HUBBUB_NEEDDATA         = 9,
	HUBBUB_BADENCODING      = 10,
	HUBBUB_UNKNOWN          = 11
} hubbub_error;
#endif

typedef enum {
	DOM_HUBBUB_OK           = 0,
	DOM_HUBBUB_NOMEM        = 1,
	DOM_HUBBUB_BADPARM      = 2,
	DOM_HUBBUB_DOM          = 3,
	DOM_HUBBUB_HUBBUB_ERR   = (1<<16),
	DOM_HUBBUB_HUBBUB_ERR_PAUSED = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_PAUSED),
	DOM_HUBBUB_HUBBUB_ERR_ENCODINGCHANGE = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_ENCODINGCHANGE),
	DOM_HUBBUB_HUBBUB_ERR_NOMEM = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_NOMEM),
	DOM_HUBBUB_HUBBUB_ERR_BADPARM = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_BADPARM),
	DOM_HUBBUB_HUBBUB_ERR_INVALID = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_INVALID),
	DOM_HUBBUB_HUBBUB_ERR_FILENOTFOUND = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_FILENOTFOUND),
	DOM_HUBBUB_HUBBUB_ERR_NEEDDATA = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_NEEDDATA),
	DOM_HUBBUB_HUBBUB_ERR_BADENCODING = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_BADENCODING),
	DOM_HUBBUB_HUBBUB_ERR_UNKNOWN = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_UNKNOWN)
} dom_hubbub_error;

#endif
