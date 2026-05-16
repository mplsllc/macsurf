/* macos9_image.c -- QuickTime Graphics Importers as NetSurf image
 * content handler. fixes78.
 *
 * Registers one shared handler for image/jpeg, image/png, image/gif,
 * image/bmp and aliases. The handler:
 *   create        -- allocate per-content state
 *   process_data  -- accumulate raw bytes into a Mac Handle
 *   data_complete -- wrap Handle in a QT data ref, ask QT for an
 *                    importer component, query natural bounds
 *   redraw        -- GraphicsImportSetGWorld + SetBoundsRect + Draw
 *                    into the active QD port (already set up by the
 *                    fixes77f offscreen composite path)
 *   destroy       -- CloseComponent + DisposeHandle
 *
 * No NetSurf bitmap intermediate. QT decodes straight into the active
 * port and respects its clipRgn for hardware clipping. */

#include "macos9.h"

/* Movies.h pulls in Components.h, the GraphicsImportComponent typedef,
 * and the pascal-tagged GraphicsImporter / Component Manager prototypes
 * we need (GetGraphicsImporterForDataRef, GraphicsImportGetNaturalBounds,
 * GraphicsImportSetGWorld, GraphicsImportSetBoundsRect,
 * GraphicsImportDraw, CloseComponent) plus HandleDataHandlerSubType.
 * We deliberately do NOT include <QuickTimeComponents.h> -- on CW8 it
 * chains into QuickTimeMusic.h which fails on undefined BigEndianLong
 * types, and we don't need the music / streaming / VR subsystems. */
#include <Movies.h>

#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "netsurf/types.h"
#include "netsurf/plotters.h"
#include "netsurf/content.h"
#include "content/content_protected.h"
#include "content/content_factory.h"

#include "macsurf_debug.h"

typedef struct macos9_qt_image_content {
	struct content base;     /* MUST be first -- NetSurf casts to this */
	Handle compressed;       /* raw bytes, grown by process_data */
	GraphicsImportComponent gi; /* importer instance; NULL until convert */
	Rect natural_bounds;     /* set at data_complete */
} macos9_qt_image_content;

static nserror
macos9_qt_image_create(const struct content_handler *handler,
		lwc_string *imime_type, const struct http_parameter *params,
		struct llcache_handle *llcache, const char *fallback_charset,
		bool quirks, struct content **c)
{
	macos9_qt_image_content *qti;
	nserror err;

	qti = (macos9_qt_image_content *)calloc(1,
			sizeof(macos9_qt_image_content));
	if (qti == NULL) {
		return NSERROR_NOMEM;
	}

	err = content__init(&qti->base, handler, imime_type, params,
			llcache, fallback_charset, quirks);
	if (err != NSERROR_OK) {
		free(qti);
		return err;
	}

	qti->compressed = NULL;
	qti->gi = NULL;
	qti->natural_bounds.top = 0;
	qti->natural_bounds.left = 0;
	qti->natural_bounds.bottom = 0;
	qti->natural_bounds.right = 0;

	*c = (struct content *)qti;
	return NSERROR_OK;
}

static bool
macos9_qt_image_process(struct content *c, const char *data,
		unsigned int size)
{
	macos9_qt_image_content *qti = (macos9_qt_image_content *)c;
	Size old_size;
	OSErr err;

	if (size == 0) {
		return true;
	}

	if (qti->compressed == NULL) {
		qti->compressed = NewHandle((Size)size);
		if (qti->compressed == NULL) {
			content_broadcast_error(c, NSERROR_NOMEM, NULL);
			return false;
		}
		old_size = 0;
	} else {
		old_size = GetHandleSize(qti->compressed);
		SetHandleSize(qti->compressed, old_size + (Size)size);
		err = MemError();
		if (err != noErr) {
			content_broadcast_error(c, NSERROR_NOMEM, NULL);
			return false;
		}
	}

	HLock(qti->compressed);
	BlockMoveData(data, (*qti->compressed) + old_size, (Size)size);
	HUnlock(qti->compressed);
	return true;
}

static bool
macos9_qt_image_convert(struct content *c)
{
	macos9_qt_image_content *qti = (macos9_qt_image_content *)c;
	Handle data_ref;
	OSErr osr;
	ComponentResult cr;
	GraphicsImportComponent importer;
	Rect bounds;

	if (qti->compressed == NULL ||
			GetHandleSize(qti->compressed) == 0) {
		content_broadcast_error(c, NSERROR_INVALID, NULL);
		return false;
	}

	data_ref = NULL;
	osr = PtrToHand(&qti->compressed, &data_ref, (long)sizeof(Handle));
	if (osr != noErr || data_ref == NULL) {
		content_broadcast_error(c, NSERROR_NOMEM, NULL);
		return false;
	}

	importer = NULL;
	cr = GetGraphicsImporterForDataRef(data_ref,
			HandleDataHandlerSubType, &importer);
	DisposeHandle(data_ref);
	if (cr != noErr || importer == NULL) {
		content_broadcast_error(c, NSERROR_INVALID, NULL);
		return false;
	}

	bounds.top = 0;
	bounds.left = 0;
	bounds.bottom = 0;
	bounds.right = 0;
	cr = GraphicsImportGetNaturalBounds(importer, &bounds);
	if (cr != noErr) {
		CloseComponent(importer);
		content_broadcast_error(c, NSERROR_INVALID, NULL);
		return false;
	}

	qti->gi = importer;
	qti->natural_bounds = bounds;

	c->width = (int)(bounds.right - bounds.left);
	c->height = (int)(bounds.bottom - bounds.top);

	MS_LOG("img convert OK");
	content_set_ready(c);
	content_set_done(c);
	content_set_status(c, "");
	return true;
}

static bool
macos9_qt_image_redraw(struct content *c, struct content_redraw_data *data,
		const struct rect *clip, const struct redraw_context *ctx)
{
	macos9_qt_image_content *qti = (macos9_qt_image_content *)c;
	Rect dst;
	CGrafPtr save_port;
	GDHandle save_gdh;

	(void)clip;
	(void)ctx;

	if (qti->gi == NULL) {
		MS_LOG("img redraw: gi NULL");
		return true;
	}

	dst.left = (short)data->x;
	dst.top = (short)data->y;
	dst.right = (short)(data->x + data->width);
	dst.bottom = (short)(data->y + data->height);

	GetGWorld(&save_port, &save_gdh);

	GraphicsImportSetGWorld(qti->gi, save_port, save_gdh);
	GraphicsImportSetBoundsRect(qti->gi, &dst);
	GraphicsImportDraw(qti->gi);
	MS_LOG("img redraw drew");

	SetGWorld(save_port, save_gdh);
	return true;
}

static void
macos9_qt_image_destroy(struct content *c)
{
	macos9_qt_image_content *qti = (macos9_qt_image_content *)c;

	if (qti->gi != NULL) {
		CloseComponent(qti->gi);
		qti->gi = NULL;
	}
	if (qti->compressed != NULL) {
		DisposeHandle(qti->compressed);
		qti->compressed = NULL;
	}
}

static nserror
macos9_qt_image_clone(const struct content *old, struct content **newc)
{
	(void)old;
	(void)newc;
	return NSERROR_CLONE_FAILED;
}

static content_type
macos9_qt_image_type(void)
{
	return CONTENT_IMAGE;
}

static bool
macos9_qt_image_is_opaque(struct content *c)
{
	(void)c;
	return false;
}

/* Vtable -- positional init only (CW8 C89, no designated initialisers).
 * Field order MUST match struct content_handler in
 * content/content_protected.h. */
static const struct content_handler macos9_qt_image_handler = {
	NULL,                          /* fini */
	macos9_qt_image_create,        /* create */
	macos9_qt_image_process,       /* process_data */
	macos9_qt_image_convert,       /* data_complete */
	NULL,                          /* reformat */
	macos9_qt_image_destroy,       /* destroy */
	NULL,                          /* stop */
	NULL,                          /* mouse_track */
	NULL,                          /* mouse_action */
	NULL,                          /* keypress */
	macos9_qt_image_redraw,        /* redraw */
	NULL,                          /* open */
	NULL,                          /* close */
	NULL,                          /* clear_selection */
	NULL,                          /* get_selection */
	NULL,                          /* get_contextual_content */
	NULL,                          /* scroll_at_point */
	NULL,                          /* drop_file_at_point */
	NULL,                          /* debug_dump */
	NULL,                          /* debug */
	macos9_qt_image_clone,         /* clone */
	NULL,                          /* matches_quirks */
	NULL,                          /* get_encoding */
	macos9_qt_image_type,          /* type */
	NULL,                          /* add_user */
	NULL,                          /* remove_user */
	NULL,                          /* exec */
	NULL,                          /* saw_insecure_objects */
	NULL,                          /* textsearch_find */
	NULL,                          /* textsearch_bounds */
	NULL,                          /* textselection_redraw */
	NULL,                          /* textselection_copy */
	NULL,                          /* textselection_get_end */
	NULL,                          /* get_internal */
	macos9_qt_image_is_opaque,     /* is_opaque */
	false                          /* no_share */
};

static const char *macos9_qt_image_mime[] = {
	"image/jpeg",
	"image/jpg",
	"image/pjpeg",
	"image/png",
	"image/x-png",
	"image/gif",
	"image/bmp",
	"image/x-bmp",
	"image/x-ms-bmp",
	"image/tiff",
	"image/x-tiff"
};

/* Replaces the no-op stub in misc_stub.c. NetSurf core's
 * desktop/netsurf.c:netsurf_init -> image_init() chain. */
nserror image_init(void)
{
	nserror err;
	size_t i;
	size_t n;

	n = sizeof(macos9_qt_image_mime) / sizeof(macos9_qt_image_mime[0]);
	for (i = 0; i < n; ++i) {
		err = content_factory_register_handler(macos9_qt_image_mime[i],
				&macos9_qt_image_handler);
		if (err != NSERROR_OK) {
			return err;
		}
	}
	return NSERROR_OK;
}
