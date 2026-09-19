/* Host-only regression test for MacSurf's vendored libwebp decoder slice. */
#include <stdio.h>
#include <stdlib.h>

#include "macwebp_decode.h"
#include "macwebp_demux.h"

static unsigned char *
read_file(const char *path, size_t *size)
{
	FILE *f;
	long length;
	unsigned char *data;
	f = fopen(path, "rb");
	if (f == NULL) return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	length = ftell(f);
	if (length <= 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	data = malloc((size_t)length);
	if (data == NULL || fread(data, 1, (size_t)length, f) != (size_t)length) {
		free(data);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*size = (size_t)length;
	return data;
}

static int
test_static(const char *path, int expect_alpha)
{
	unsigned char *data;
	unsigned char *pixels;
	size_t size;
	WebPBitstreamFeatures features;
	VP8StatusCode status;
	int ok;
	data = read_file(path, &size);
	if (data == NULL) return 0;
	status = WebPGetFeatures(data, size, &features);
	ok = status == VP8_STATUS_OK && features.width == 4 && features.height == 3 &&
		features.has_animation == 0 && features.has_alpha == expect_alpha;
	pixels = NULL;
	if (ok) pixels = WebPDecodeRGBA(data, size, NULL, NULL);
	if (pixels == NULL) ok = 0;
	WebPFree(pixels);
	free(data);
	return ok;
}

static int
test_animation(const char *path)
{
	unsigned char *data;
	uint8_t *frame;
	size_t size;
	WebPData source;
	WebPAnimDecoderOptions options;
	WebPAnimDecoder *decoder;
	WebPAnimInfo info;
	int timestamp;
	int frames;
	int previous;
	int ok;
	data = read_file(path, &size);
	if (data == NULL) return 0;
	source.bytes = data;
	source.size = size;
	ok = WebPAnimDecoderOptionsInit(&options) != 0;
	options.color_mode = MODE_RGBA;
	options.use_threads = 0;
	decoder = ok ? WebPAnimDecoderNew(&source, &options) : NULL;
	ok = decoder != NULL && WebPAnimDecoderGetInfo(decoder, &info) != 0 &&
		info.canvas_width == 4 && info.canvas_height == 3 && info.frame_count == 2;
	frames = 0;
	previous = 0;
	while (ok && WebPAnimDecoderHasMoreFrames(decoder)) {
		frame = NULL;
		timestamp = 0;
		if (!WebPAnimDecoderGetNext(decoder, &frame, &timestamp) || frame == NULL ||
			timestamp <= previous) {
			ok = 0;
			break;
		}
		previous = timestamp;
		frames++;
	}
	if (frames != 2) ok = 0;
	WebPAnimDecoderDelete(decoder);
	free(data);
	return ok;
}

int
main(void)
{
	const unsigned char malformed[] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0,
		'W', 'E', 'B', 'P' };
	WebPBitstreamFeatures features;
	int ok;
	ok = test_static("webp-corpus/static-vp8.webp", 0) &&
		test_static("webp-corpus/static-alpha-vp8l.webp", 1) &&
		test_animation("webp-corpus/animated-alpha.webp") &&
		WebPGetFeatures(malformed, sizeof(malformed), &features) != VP8_STATUS_OK;
	if (!ok) {
		fprintf(stderr, "webp codec regression failed\n");
		return 1;
	}
	puts("webp codec regression passed");
	return 0;
}
