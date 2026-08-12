/*
 * MacSurf - dom/bindings/hubbub/errors.h (netsurf/include mirror)
 *
 * Duplicate of browser/libdom/include/dom/bindings/hubbub/errors.h so that
 * the browser:netsurf:include: access path (which CW8 always has) can also
 * resolve <dom/bindings/hubbub/errors.h>.  The browser:libdom:include: path
 * resolves it too, but belt-and-suspenders given the HFS+ path quirks.
 */
#ifndef dom_hubbub_errors_h_
#define dom_hubbub_errors_h_

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
