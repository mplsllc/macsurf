/*
 * Copyright 2008 Sean Fox <dyntryx@gmail.com>
 * Copyright 2008 James Bursa <james@netsurf-browser.org>
 *
 * This file is part of NetSurf's libnsbmp, http://www.netsurf-browser.org/
 * Licenced under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 */

/**
 * \file
 * Use libnsbmp to decode icons into ppm or pam files for testing
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

/* Currently the library returns the data in RGBA format,
 * so there are 4 bytes per pixel */
#define BYTES_PER_PIXEL sizeof(uint32_t)

/* White with alpha masking. */
#define TRANSPARENT_COLOR 0xffffffff

static void *bitmap_create(int width, int height, unsigned int state)
{
        (void) state;  /* unused */
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


static void write_pam(FILE* fh, const char *name, struct bmp_image *bmp)
{
        uint16_t row, col;
        uint8_t *image;

        fprintf(fh, "P7\n");
        fprintf(fh, "# %s\n", name);
        fprintf(fh, "WIDTH %u\n", bmp->width);
        fprintf(fh, "HEIGHT %u\n", bmp->height);
        fprintf(fh, "DEPTH 4\n");
        fprintf(fh, "MAXVAL 255\n");
        fprintf(fh, "TUPLTYPE RGB_ALPHA\n");
        fprintf(fh, "ENDHDR\n");


        image = (uint8_t *) bmp->bitmap;
        for (row = 0; row != bmp->height; row++) {
                for (col = 0; col != bmp->width; col++) {
                        size_t z = (row * bmp->width + col) * BYTES_PER_PIXEL;
                        putc(image[z], fh);
                        putc(image[z + 1], fh);
                        putc(image[z + 2], fh);
                        putc(image[z + 3], fh);
                }
        }

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

int main(int argc, char *argv[])
{
        bmp_bitmap_callback_vt bitmap_callbacks = {
                bitmap_create,
                bitmap_destroy,
                bitmap_get_buffer,
        };
        uint16_t width, height;
        ico_collection ico;
        bmp_result code;
        struct bmp_image *bmp;
        size_t size;
        unsigned short res = 0;
        unsigned char *data;
        FILE *outf = stdout;

        if ((argc < 2) || (argc > 5)) {
                fprintf(stderr, "Usage: %s collection.ico [width=255] [height=255] [outfile]\n", argv[0]);
                return 1;
        }
        width = (argc >= 3) ?  atoi(argv[2]) : 255;
        height = (argc >= 4) ? atoi(argv[3]) : 255;

        if (argc >= 5) {
                outf = fopen(argv[4], "w+");
                if (outf == NULL) {
                        fprintf(stderr, "Unable to open %s for writing\n", argv[2]);
                        return 2;
                }
        }

        /* create our bmp image */
        ico_collection_create(&ico, &bitmap_callbacks);

        /* load file into memory */
        data = load_file(argv[1], &size);

        /* analyse the BMP */
        code = ico_analyse(&ico, size, data);
        if (code != BMP_OK) {
                warning("ico_analyse", code);
                res = 3;
                goto cleanup;
        }

        /* decode the image */
        bmp = ico_find(&ico, width, height);
        assert(bmp);

        code = bmp_decode(bmp);
        /* code = bmp_decode_trans(bmp, TRANSPARENT_COLOR); */
        if (code != BMP_OK) {
                warning("bmp_decode", code);
                /* allow partially decoded images */
                if ((code != BMP_INSUFFICIENT_DATA) &&
                    (code != BMP_DATA_ERROR)) {
                        res = 1;
                        goto cleanup;
                }

                /* skip if the decoded image would be ridiculously large */
                if ((bmp->width * bmp->height) > 200000) {
                        res = 1;
                        goto cleanup;
                }
        }

        if (bmp->opaque) {
                write_ppm(outf, argv[1], bmp);
        } else {
                write_pam(outf, argv[1], bmp);
        }

        if (argc >= 5) {
                fclose(outf);
        }

cleanup:
        /* clean up */
        ico_finalise(&ico);
        free(data);

        return res;
}
