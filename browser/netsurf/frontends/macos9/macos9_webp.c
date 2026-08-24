/* macos9_webp.c - libwebp-backed image/webp content for MacSurf.
 *
 * This deliberately does not use NetSurf's generic image cache: macos9_image
 * owns a deferred, display-size bitmap policy and the generic handlers are
 * excluded from this frontend's build.  libwebp supplies all VP8, VP8L, alpha
 * and animation composition; MacSurf owns bitmap lifetime and scheduling. */

#include "macos9.h"

#include <stdlib.h>
#include <string.h>

#include "utils/ns_errors.h"
#include "utils/nsoption.h"
#include "netsurf/types.h"
#include "netsurf/plotters.h"
#include "netsurf/content.h"
#include "netsurf/bitmap.h"
#include "content/content_protected.h"
#include "content/content.h"
#include "content/llcache.h"
#include "content/content_factory.h"
#include "desktop/gui_internal.h"
#include "desktop/gui_table.h"

#include "macsurf_debug.h"
#include "macos9_webp.h"

#include "macwebp_decode.h"
#include "macwebp_demux.h"

#define MACOS9_WEBP_IMAGE_MAX_BYTES (16L * 1024L * 1024L)
#define MACOS9_WEBP_TOTAL_MAX_BYTES (32L * 1024L * 1024L)
#define MACOS9_WEBP_ANIM_BYTES_PER_PIXEL 12L

extern long macsurf__decoded_img_bytes_current;
extern void macos9_image_finish_rgba_bitmap(void *bitmap,
		unsigned char *mask, int mask_rowbytes, bool has_transparency);
extern void macos9_animation_register_rect(int x, int y, int w, int h);

typedef struct macos9_webp_content macos9_webp_content;
struct macos9_webp_content {
	struct content base;
	WebPAnimDecoder *anim_decoder;
	WebPAnimInfo anim_info;
	void *bitmap;
	long reserved_bytes;
	int bitmap_w;
	int bitmap_h;
	int timestamp;
	unsigned long completed_loops;
	bool animated;
	bool has_alpha;
	bool animation_running;
	bool frame_ready;
	macos9_webp_content *prev;
	macos9_webp_content *next;
};

static macos9_webp_content *macos9_webp_head = NULL;

static void macos9_webp_animation_callback(void *p);

static void
macos9_webp_link(macos9_webp_content *webp)
{
	webp->prev = NULL;
	webp->next = macos9_webp_head;
	if (macos9_webp_head != NULL) macos9_webp_head->prev = webp;
	macos9_webp_head = webp;
}

static void
macos9_webp_unlink(macos9_webp_content *webp)
{
	if (webp->prev != NULL) webp->prev->next = webp->next;
	else if (macos9_webp_head == webp) macos9_webp_head = webp->next;
	if (webp->next != NULL) webp->next->prev = webp->prev;
	webp->prev = NULL;
	webp->next = NULL;
}

static void
macos9_webp_drop_decoded(macos9_webp_content *webp)
{
	if (webp->anim_decoder != NULL) {
		WebPAnimDecoderDelete(webp->anim_decoder);
		webp->anim_decoder = NULL;
	}
	if (webp->bitmap != NULL && guit != NULL && guit->bitmap != NULL &&
		guit->bitmap->destroy != NULL) {
		guit->bitmap->destroy(webp->bitmap);
	}
	webp->bitmap = NULL;
	webp->bitmap_w = 0;
	webp->bitmap_h = 0;
	webp->frame_ready = false;
	if (webp->reserved_bytes > 0) {
		macsurf__decoded_img_bytes_current -= webp->reserved_bytes;
		if (macsurf__decoded_img_bytes_current < 0) {
			macsurf__decoded_img_bytes_current = 0;
		}
		webp->reserved_bytes = 0;
	}
}

static bool
macos9_webp_reserve(macos9_webp_content *webp, long bytes)
{
	if (bytes <= 0 || bytes > MACOS9_WEBP_IMAGE_MAX_BYTES ||
		macsurf__decoded_img_bytes_current + bytes >
		MACOS9_WEBP_TOTAL_MAX_BYTES) {
		macsurf_debug_log_writef("webp reject: decode reservation %ld", bytes);
		return false;
	}
	macsurf__decoded_img_bytes_current += bytes;
	webp->reserved_bytes = bytes;
	return true;
}

static bool
macos9_webp_dimensions_ok(uint32_t width, uint32_t height, long bytes_per_pixel,
		long *out_bytes)
{
	long pixels;
	long bytes;
	if (width == 0 || height == 0 || width > 32768U || height > 32768U) {
		return false;
	}
	if ((unsigned long)width > 0x7fffffffUL / (unsigned long)height) {
		return false;
	}
	pixels = (long)width * (long)height;
	if (pixels > 0x7fffffffL / bytes_per_pixel) return false;
	bytes = pixels * bytes_per_pixel;
	*out_bytes = bytes;
	return true;
}

static void
macos9_webp_fit_to_cap(int *width, int *height)
{
	long scale;
	long candidate_w;
	long candidate_h;
	if (*width <= 0 || *height <= 0) return;
	if ((long)*width <= MACOS9_WEBP_IMAGE_MAX_BYTES / 4L /
		(long)*height) return;
	scale = 1000L;
	for (;;) {
		candidate_w = (long)*width * scale / 1000L;
		candidate_h = (long)*height * scale / 1000L;
		if (candidate_w > 0 && candidate_h > 0 &&
			candidate_w <= MACOS9_WEBP_IMAGE_MAX_BYTES / 4L /
			candidate_h) break;
		if (scale <= 1) break;
		scale -= 10L;
	}
	*width = (int)((long)*width * scale / 1000L);
	*height = (int)((long)*height * scale / 1000L);
	if (*width < 1) *width = 1;
	if (*height < 1) *height = 1;
}

static bool
macos9_webp_finish_alpha(void *bitmap, int width, int height)
{
	unsigned char *pixels;
	unsigned char *mask;
	long rowbytes;
	int mask_rowbytes;
	int x, y;
	bool transparent;
	if (bitmap == NULL || guit == NULL || guit->bitmap == NULL ||
		guit->bitmap->get_buffer == NULL ||
		guit->bitmap->get_rowstride == NULL) return false;
	pixels = (unsigned char *)guit->bitmap->get_buffer(bitmap);
	rowbytes = (long)guit->bitmap->get_rowstride(bitmap);
	if (pixels == NULL || rowbytes < (long)width * 4L) return false;
	mask_rowbytes = ((width + 15) / 16) * 2;
	mask = calloc((size_t)mask_rowbytes * (size_t)height, 1);
	if (mask == NULL) return false;
	transparent = false;
	for (y = 0; y < height; y++) {
		unsigned char *row = pixels + (long)y * rowbytes;
		unsigned char *mask_row = mask + (long)y * mask_rowbytes;
		for (x = 0; x < width; x++) {
			if (row[x * 4 + 3] >= 8) {
				mask_row[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
			} else {
				transparent = true;
			}
		}
	}
	macos9_image_finish_rgba_bitmap(bitmap, mask, mask_rowbytes, transparent);
	return true;
}

static bool
macos9_webp_decode_static(macos9_webp_content *webp, int target_w, int target_h)
{
	const uint8_t *source;
	size_t source_size;
	void *bitmap;
	unsigned char *pixels;
	long rowbytes;
	long bytes;
	WebPDecoderConfig config;
	VP8StatusCode status;
	int width;
	int height;

	source = content__get_source_data(&webp->base, &source_size);
	if (source == NULL || source_size == 0) return false;
	width = target_w;
	height = target_h;
	if (width <= 0 || width > (int)webp->base.width) width = (int)webp->base.width;
	if (height <= 0 || height > (int)webp->base.height) height = (int)webp->base.height;
	macos9_webp_fit_to_cap(&width, &height);
	if (!macos9_webp_dimensions_ok((uint32_t)width, (uint32_t)height, 4L,
			&bytes)) return false;
	if (webp->bitmap != NULL && webp->bitmap_w == width &&
		webp->bitmap_h == height) return true;
	macos9_webp_drop_decoded(webp);
	if (!macos9_webp_reserve(webp, bytes)) return false;
	bitmap = guit->bitmap->create(width, height, BITMAP_CLEAR);
	if (bitmap == NULL) {
		macos9_webp_drop_decoded(webp);
		return false;
	}
	pixels = (unsigned char *)guit->bitmap->get_buffer(bitmap);
	rowbytes = (long)guit->bitmap->get_rowstride(bitmap);
	if (pixels == NULL || rowbytes < (long)width * 4L ||
		!WebPInitDecoderConfig(&config)) {
		guit->bitmap->destroy(bitmap);
		macos9_webp_drop_decoded(webp);
		return false;
	}
	config.output.colorspace = MODE_RGBA;
	config.output.is_external_memory = 1;
	config.output.u.RGBA.rgba = pixels;
	config.output.u.RGBA.stride = (int)rowbytes;
	config.output.u.RGBA.size = (size_t)rowbytes * (size_t)height;
	if (width != (int)webp->base.width || height != (int)webp->base.height) {
		config.options.use_scaling = 1;
		config.options.scaled_width = width;
		config.options.scaled_height = height;
	}
	config.options.use_threads = 0;
	status = WebPDecode(source, source_size, &config);
	WebPFreeDecBuffer(&config.output);
	if (status != VP8_STATUS_OK || !macos9_webp_finish_alpha(bitmap, width, height)) {
		guit->bitmap->destroy(bitmap);
		macos9_webp_drop_decoded(webp);
		return false;
	}
	webp->bitmap = bitmap;
	webp->bitmap_w = width;
	webp->bitmap_h = height;
	return true;
}

static bool
macos9_webp_copy_animation_frame(macos9_webp_content *webp, const uint8_t *frame)
{
	unsigned char *pixels;
	long rowbytes;
	int y;
	if (webp->bitmap == NULL || frame == NULL || guit == NULL ||
		guit->bitmap == NULL || guit->bitmap->get_buffer == NULL ||
		guit->bitmap->get_rowstride == NULL) return false;
	pixels = (unsigned char *)guit->bitmap->get_buffer(webp->bitmap);
	rowbytes = (long)guit->bitmap->get_rowstride(webp->bitmap);
	if (pixels == NULL || rowbytes < (long)webp->bitmap_w * 4L) return false;
	for (y = 0; y < webp->bitmap_h; y++) {
		memcpy(pixels + (long)y * rowbytes,
			frame + (long)y * webp->bitmap_w * 4L,
			(size_t)webp->bitmap_w * 4U);
	}
	if (!macos9_webp_finish_alpha(webp->bitmap, webp->bitmap_w,
		webp->bitmap_h)) return false;
	webp->frame_ready = true;
	return true;
}

static bool
macos9_webp_ensure_animation(macos9_webp_content *webp)
{
	const uint8_t *source;
	size_t source_size;
	WebPData data;
	WebPAnimDecoderOptions options;
	long reserve;
	if (webp->anim_decoder != NULL) return true;
	if (guit == NULL || guit->bitmap == NULL || guit->bitmap->create == NULL ||
		guit->bitmap->get_buffer == NULL || guit->bitmap->get_rowstride == NULL ||
		guit->bitmap->destroy == NULL) return false;
	if (!macos9_webp_dimensions_ok(webp->base.width, webp->base.height,
			MACOS9_WEBP_ANIM_BYTES_PER_PIXEL, &reserve) ||
		!macos9_webp_reserve(webp, reserve)) return false;
	source = content__get_source_data(&webp->base, &source_size);
	if (source == NULL || source_size == 0 ||
		!WebPAnimDecoderOptionsInit(&options)) {
		macos9_webp_drop_decoded(webp);
		return false;
	}
	data.bytes = source;
	data.size = source_size;
	options.color_mode = MODE_RGBA;
	options.use_threads = 0;
	webp->anim_decoder = WebPAnimDecoderNew(&data, &options);
	if (webp->anim_decoder == NULL ||
		!WebPAnimDecoderGetInfo(webp->anim_decoder, &webp->anim_info)) {
		macos9_webp_drop_decoded(webp);
		return false;
	}
	webp->bitmap = guit->bitmap->create((int)webp->anim_info.canvas_width,
		(int)webp->anim_info.canvas_height, BITMAP_CLEAR);
	if (webp->bitmap == NULL) {
		macos9_webp_drop_decoded(webp);
		return false;
	}
	webp->bitmap_w = (int)webp->anim_info.canvas_width;
	webp->bitmap_h = (int)webp->anim_info.canvas_height;
	webp->timestamp = 0;
	webp->completed_loops = 0;
	return true;
}

static bool
macos9_webp_advance_animation(macos9_webp_content *webp, bool broadcast)
{
	uint8_t *frame;
	int timestamp;
	int delay;
	union content_msg_data redraw;
	if (!macos9_webp_ensure_animation(webp)) return false;
	if (!WebPAnimDecoderHasMoreFrames(webp->anim_decoder)) {
		if (webp->anim_info.loop_count != 0 &&
			webp->completed_loops + 1 >= webp->anim_info.loop_count) {
			return true;
		}
		webp->completed_loops++;
		WebPAnimDecoderReset(webp->anim_decoder);
		webp->timestamp = 0;
	}
	frame = NULL;
	timestamp = 0;
	if (!WebPAnimDecoderGetNext(webp->anim_decoder, &frame, &timestamp) ||
		!macos9_webp_copy_animation_frame(webp, frame)) return false;
	delay = timestamp - webp->timestamp;
	webp->timestamp = timestamp;
	if (delay < 10) delay = 10;
	if (webp->animation_running && nsoption_bool(animate_images) &&
		content_count_users(&webp->base) > 0) {
		macos9_schedule(delay, macos9_webp_animation_callback, webp);
	}
	if (broadcast) {
		redraw.redraw.x = 0;
		redraw.redraw.y = 0;
		redraw.redraw.width = webp->bitmap_w;
		redraw.redraw.height = webp->bitmap_h;
		content_broadcast(&webp->base, CONTENT_MSG_REDRAW, &redraw);
	}
	return true;
}

static void
macos9_webp_animation_callback(void *p)
{
	macos9_webp_content *webp = (macos9_webp_content *)p;
	if (webp == NULL || !webp->animation_running) return;
	if (!nsoption_bool(animate_images)) {
		webp->animation_running = false;
		return;
	}
	(void)macos9_webp_advance_animation(webp, true);
}

/* Start at frame zero.  add_user can run before conversion discovers ANIM,
 * so conversion must also use this path once the feature bits are known. */
static void
macos9_webp_start_animation(macos9_webp_content *webp, bool broadcast)
{
	if (!webp->animated || !nsoption_bool(animate_images)) return;
	webp->animation_running = true;
	if (webp->anim_decoder != NULL) WebPAnimDecoderReset(webp->anim_decoder);
	webp->timestamp = 0;
	webp->completed_loops = 0;
	webp->frame_ready = false;
	(void)macos9_webp_advance_animation(webp, broadcast);
}

static nserror
macos9_webp_create(const struct content_handler *handler,
		lwc_string *imime_type, const struct http_parameter *params,
		struct llcache_handle *llcache, const char *fallback_charset,
		bool quirks, struct content **c)
{
	macos9_webp_content *webp;
	nserror err;
	webp = calloc(1, sizeof(*webp));
	if (webp == NULL) return NSERROR_NOMEM;
	err = content__init(&webp->base, handler, imime_type, params, llcache,
		fallback_charset, quirks);
	if (err != NSERROR_OK) {
		free(webp);
		return err;
	}
	macos9_webp_link(webp);
	*c = &webp->base;
	return NSERROR_OK;
}

static bool
macos9_webp_process_data(struct content *c, const char *data, unsigned int size)
{
	(void)c;
	(void)data;
	(void)size;
	return true;
}

static bool
macos9_webp_convert(struct content *c)
{
	macos9_webp_content *webp = (macos9_webp_content *)c;
	const uint8_t *source;
	size_t source_size;
	WebPBitstreamFeatures features;
	VP8StatusCode status;
	source = content__get_source_data(c, &source_size);
	if (source == NULL || source_size == 0) return false;
	status = WebPGetFeatures(source, source_size, &features);
	if (status != VP8_STATUS_OK || features.width <= 0 || features.height <= 0) {
		macsurf_debug_log_writef("webp reject: invalid bitstream status=%d",
			(int)status);
		return false;
	}
	{
		long decoded_bytes;
		if (!macos9_webp_dimensions_ok((uint32_t)features.width,
				(uint32_t)features.height, 4L, &decoded_bytes)) {
			macsurf_debug_log_writef("webp reject: dimensions %dx%d",
				features.width, features.height);
			return false;
		}
		webp->base.size = (unsigned)decoded_bytes;
	}
	webp->base.width = (unsigned)features.width;
	webp->base.height = (unsigned)features.height;
	webp->animated = features.has_animation ? true : false;
	webp->has_alpha = features.has_alpha ? true : false;
	/* fixes1319: per-decode success trace removed - object.c's
	 * macsurf_log_final_image() now reports every loaded image, WebP
	 * included, from one place. */
	macos9_webp_start_animation(webp, false);
	content_set_ready(c);
	content_set_done(c);
	return true;
}

static bool
macos9_webp_redraw(struct content *c, struct content_redraw_data *data,
		const struct rect *clip, const struct redraw_context *ctx)
{
	macos9_webp_content *webp = (macos9_webp_content *)c;
	bitmap_flags_t flags;
	int draw_w;
	int draw_h;
	(void)clip;
	if (ctx == NULL || ctx->plot == NULL || ctx->plot->bitmap == NULL) return true;
	draw_w = data->width;
	draw_h = data->height;
	if (draw_w <= 0) draw_w = (int)c->width;
	if (draw_h <= 0) draw_h = (int)c->height;
	if (webp->animated) {
		if (!macos9_webp_ensure_animation(webp) ||
			(!webp->frame_ready && !macos9_webp_advance_animation(webp, false))) {
			return false;
		}
	} else {
		macos9_webp_fit_to_cap(&draw_w, &draw_h);
		if (!macos9_webp_decode_static(webp, draw_w, draw_h)) return false;
	}
	if (webp->bitmap == NULL) return false;
	/* The image callback updates pixels on its own schedule.  Keep this
	 * rectangle in the frontend's idle repaint pump so the new frame reaches
	 * the window even when no user input (such as scrolling) occurs. */
	if (webp->animated && webp->animation_running) {
		macos9_animation_register_rect(data->x, data->y, draw_w, draw_h);
	}
	flags = BITMAPF_NONE;
	if (data->repeat_x) flags |= BITMAPF_REPEAT_X;
	if (data->repeat_y) flags |= BITMAPF_REPEAT_Y;
	if (data->nearest) flags |= BITMAPF_NEAREST;
	flags |= BITMAPF_BLEND_MODE(data->background_blend_mode);
	return ctx->plot->bitmap(ctx, (struct bitmap *)webp->bitmap,
		data->x, data->y, draw_w, draw_h, data->background_colour,
		flags) == NSERROR_OK;
}

static void
macos9_webp_destroy(struct content *c)
{
	macos9_webp_content *webp = (macos9_webp_content *)c;
	macos9_schedule_cancel_owner(webp);
	webp->animation_running = false;
	macos9_webp_drop_decoded(webp);
	macos9_webp_unlink(webp);
}

static nserror
macos9_webp_clone(const struct content *old, struct content **newc)
{
	macos9_webp_content *webp;
	nserror err;
	webp = calloc(1, sizeof(*webp));
	if (webp == NULL) return NSERROR_NOMEM;
	err = content__clone(old, &webp->base);
	if (err != NSERROR_OK) {
		free(webp);
		return err;
	}
	macos9_webp_link(webp);
	if (old->status == CONTENT_STATUS_READY || old->status == CONTENT_STATUS_DONE) {
		if (!macos9_webp_convert(&webp->base)) {
			macos9_webp_unlink(webp);
			content_destroy(&webp->base);
			return NSERROR_CLONE_FAILED;
		}
	}
	*newc = &webp->base;
	return NSERROR_OK;
}

static content_type
macos9_webp_type(void)
{
	return CONTENT_IMAGE;
}

static bool
macos9_webp_is_opaque(struct content *c)
{
	macos9_webp_content *webp = (macos9_webp_content *)c;
	return webp->has_alpha ? false : true;
}

static void
macos9_webp_add_user(struct content *c)
{
	macos9_webp_content *webp = (macos9_webp_content *)c;
	if (!webp->animated || content_count_users(c) != 1) return;
	macos9_webp_start_animation(webp, true);
}

static void
macos9_webp_remove_user(struct content *c)
{
	macos9_webp_content *webp = (macos9_webp_content *)c;
	if (content_count_users(c) == 1) {
		webp->animation_running = false;
		macos9_schedule_cancel_owner(webp);
	}
}

static const struct content_handler macos9_webp_handler = {
	NULL,                          /* fini */
	macos9_webp_create,            /* create */
	macos9_webp_process_data,      /* process_data */
	macos9_webp_convert,           /* data_complete */
	NULL,                          /* reformat */
	macos9_webp_destroy,           /* destroy */
	NULL,                          /* stop */
	NULL,                          /* mouse_track */
	NULL,                          /* mouse_action */
	NULL,                          /* keypress */
	macos9_webp_redraw,            /* redraw */
	NULL,                          /* open */
	NULL,                          /* close */
	NULL,                          /* clear_selection */
	NULL,                          /* get_selection */
	NULL,                          /* get_contextual_content */
	NULL,                          /* scroll_at_point */
	NULL,                          /* drop_file_at_point */
	NULL,                          /* debug_dump */
	NULL,                          /* debug */
	macos9_webp_clone,             /* clone */
	NULL,                          /* matches_quirks */
	NULL,                          /* get_encoding */
	macos9_webp_type,              /* type */
	macos9_webp_add_user,          /* add_user */
	macos9_webp_remove_user,       /* remove_user */
	NULL,                          /* exec */
	NULL,                          /* saw_insecure_objects */
	NULL,                          /* textsearch_find */
	NULL,                          /* textsearch_bounds */
	NULL,                          /* textselection_redraw */
	NULL,                          /* textselection_copy */
	NULL,                          /* textselection_get_end */
	NULL,                          /* get_internal */
	macos9_webp_is_opaque,         /* is_opaque */
	false                          /* no_share */
};

nserror
macos9_webp_init(void)
{
	nserror err;

	err = content_factory_register_handler("image/webp", &macos9_webp_handler);
	if (err != NSERROR_OK)
		return err;
	/* A few older image origins still use this non-standard alias.  Treat it
	 * identically so a valid WebP payload does not become a download merely
	 * because of its response header. */
	return content_factory_register_handler("image/x-webp", &macos9_webp_handler);
}

void
macos9_webp_purge_decoded_images(void)
{
	macos9_webp_content *webp;
	macos9_webp_content *next;
	for (webp = macos9_webp_head; webp != NULL; webp = next) {
		next = webp->next;
		macos9_schedule_cancel_owner(webp);
		webp->animation_running = false;
		macos9_webp_drop_decoded(webp);
	}
}
