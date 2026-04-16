/*
 * Copyright 2008 Sean Fox <dyntryx@gmail.com>
 * Copyright 2008 James Bursa <james@netsurf-browser.org>
 *
 * This file is part of NetSurf's libnsbmp, http://www.netsurf-browser.org/
 * Licenced under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "../include/libnsbmp.h"

#define BYTES_PER_PIXEL sizeof(uint32_t)
#define MAX_IMAGE_BYTES (48 * 1024 * 1024)
#define TRANSPARENT_COLOR 0xffffffff


static void *bitmap_create(int width, int height, unsigned int state)
{
        (void) state;  /* unused */
	/* Ensure a stupidly large bitmap is not created */
	if (width > 4096 || height > 4096) {
                return NULL;
        }
        return calloc(width * height, BYTES_PER_PIXEL);
}


static unsigned char *bitmap_get_buffer(void *bitmap)
{
        assert(bitmap);
        return bitmap;
}


static void bitmap_destroy(void *bitmap)
{
        assert(bitmap);
        free(bitmap);
}

static void warning(const char *context, bmp_result code)
{
        fprintf(stderr, "%s failed: ", context);
        switch (code) {
                case BMP_INSUFFICIENT_MEMORY:
                        fprintf(stderr, "BMP_INSUFFICIENT_MEMORY");
                        break;
                case BMP_INSUFFICIENT_DATA:
                        fprintf(stderr, "BMP_INSUFFICIENT_DATA");
                        break;
                case BMP_DATA_ERROR:
                        fprintf(stderr, "BMP_DATA_ERROR");
                        break;
                default:
                        fprintf(stderr, "unknown code %i", code);
                        break;
        }
        fprintf(stderr, "\n");
}

static void write_ppm(FILE* fh, const char *name, struct bmp_image *bmp)
{
        uint16_t row, col;
        uint8_t *image;

        fprintf(fh, "P3\n");
        fprintf(fh, "# %s\n", name);
        fprintf(fh, "# width                %u \n", bmp->width);
        fprintf(fh, "# height               %u \n", bmp->height);
        fprintf(fh, "%u %u 256\n", bmp->width, bmp->height);

        image = (uint8_t *) bmp->bitmap;
        for (row = 0; row != bmp->height; row++) {
                for (col = 0; col != bmp->width; col++) {
                        size_t z = (row * bmp->width + col) * BYTES_PER_PIXEL;
                        fprintf(fh, "%u %u %u ",
                                image[z],
                                image[z + 1],
                                image[z + 2]);
                }
                fprintf(fh, "\n");
        }
}


static unsigned char *load_file(const char *path, size_t *data_size)
{
        FILE *fd;
        struct stat sb;
        unsigned char *buffer;
        size_t size;
        size_t n;

        fd = fopen(path, "rb");
        if (!fd) {
                perror(path);
                exit(EXIT_FAILURE);
        }

        if (stat(path, &sb)) {
                perror(path);
                exit(EXIT_FAILURE);
        }
        size = sb.st_size;

        buffer = malloc(size);
        if (!buffer) {
                fprintf(stderr, "Unable to allocate %lld bytes\n",
                                (long long) size);
                exit(EXIT_FAILURE);
        }

        n = fread(buffer, 1, size, fd);
        if (n != size) {
                perror(path);
                exit(EXIT_FAILURE);
        }

        fclose(fd);

        *data_size = size;
        return buffer;
}


int main(int argc, char *argv[])
{
        bmp_bitmap_callback_vt bitmap_callbacks = {
                bitmap_create,
                bitmap_destroy,
                bitmap_get_buffer,
        };
        bmp_result code;
        bmp_image bmp;
        size_t size;
        unsigned short res = 0;
        FILE *outf = stdout;

        if (argc < 2) {
                fprintf(stderr, "Usage: %s image.bmp [out]\n", argv[0]);
                return 1;
        }

        if (argc > 2) {
                outf = fopen(argv[2], "w+");
                if (outf == NULL) {
                        fprintf(stderr, "Unable to open %s for writing\n", argv[2]);
                        return 2;
                }
        }

        /* create our bmp image */
        bmp_create(&bmp, &bitmap_callbacks);

        /* load file into memory */
        unsigned char *data = load_file(argv[1], &size);

        /* analyse the BMP */
        code = bmp_analyse(&bmp, size, data);
        if (code != BMP_OK) {
                warning("bmp_analyse", code);
                res = 3;
                goto cleanup;
        }

        /* decode the image */
        code = bmp_decode(&bmp);
        /* code = bmp_decode_trans(&bmp, TRANSPARENT_COLOR); */
        if (code != BMP_OK) {
                warning("bmp_decode", code);
                /* allow partially decoded images */
                if ((code != BMP_INSUFFICIENT_DATA) &&
                    (code != BMP_DATA_ERROR)) {
                        res = 4;
                        goto cleanup;
                }

                /* skip if the decoded image would be ridiculously large */
                if ((bmp.width * bmp.height) > 200000) {
                        res = 5;
                        goto cleanup;
                }
        }

        write_ppm(outf, argv[1], &bmp);

        if (argc > 2) {
                fclose(outf);
        }

cleanup:
        /* clean up */
        bmp_finalise(&bmp);
        free(data);

        return res;
}






