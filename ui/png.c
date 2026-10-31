/*
 * PNG encoder that needs only zlib
 *
 * Writes 8-bit RGB images with one IDAT chunk.  Each row uses the
 * "sub" filter, which suits screen contents well and is cheap.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "ui/png.h"
#include <zlib.h>

#define PNG_FILTER_SUB 1

static void png_put_be32(GByteArray *out, uint32_t v)
{
    uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v };

    g_byte_array_append(out, b, sizeof(b));
}

static void png_put_chunk(GByteArray *out, const char *type,
                          const uint8_t *data, uint32_t len)
{
    uint32_t crc;

    png_put_be32(out, len);
    g_byte_array_append(out, (const uint8_t *)type, 4);
    if (len) {
        g_byte_array_append(out, data, len);
    }
    crc = crc32(0, (const Bytef *)type, 4);
    if (len) {
        crc = crc32(crc, data, len);
    }
    png_put_be32(out, crc);
}

/* Feed @len bytes into the deflate stream, collecting output in @idat */
static bool png_deflate(z_stream *zs, GByteArray *idat, const uint8_t *in,
                        size_t len, int flush)
{
    uint8_t buf[16384];
    int ret;

    zs->next_in = (Bytef *)in;
    zs->avail_in = len;
    do {
        zs->next_out = buf;
        zs->avail_out = sizeof(buf);
        ret = deflate(zs, flush);
        if (ret == Z_STREAM_ERROR) {
            return false;
        }
        g_byte_array_append(idat, buf, sizeof(buf) - zs->avail_out);
    } while (zs->avail_out == 0 || (flush == Z_FINISH && ret != Z_STREAM_END));

    return true;
}

GByteArray *qemu_png_encode(pixman_image_t *image, Error **errp)
{
    int width = pixman_image_get_width(image);
    int height = pixman_image_get_height(image);
    size_t rowlen = 1 + (size_t)width * 3;
    g_autoptr(pixman_image_t) linebuf =
        qemu_pixman_linebuf_create(PIXMAN_BE_r8g8b8, width);
    const uint8_t *rgb = (const uint8_t *)pixman_image_get_data(linebuf);
    g_autofree uint8_t *row = g_malloc(rowlen);
    g_autoptr(GByteArray) idat = g_byte_array_new();
    GByteArray *out;
    uint8_t ihdr[13];
    z_stream zs = { 0 };
    int x, y;

    if (width <= 0 || height <= 0) {
        error_setg(errp, "cannot encode an empty image");
        return NULL;
    }
    if (deflateInit(&zs, 6) != Z_OK) {
        error_setg(errp, "PNG: deflateInit failed");
        return NULL;
    }

    for (y = 0; y < height; y++) {
        qemu_pixman_linebuf_fill(linebuf, image, width, 0, y);
        row[0] = PNG_FILTER_SUB;
        memcpy(row + 1, rgb, 3);
        for (x = 3; x < width * 3; x++) {
            row[1 + x] = rgb[x] - rgb[x - 3];
        }
        if (!png_deflate(&zs, idat, row, rowlen, Z_NO_FLUSH)) {
            goto fail;
        }
    }
    if (!png_deflate(&zs, idat, NULL, 0, Z_FINISH)) {
        goto fail;
    }
    deflateEnd(&zs);

    out = g_byte_array_sized_new(idat->len + 64);
    g_byte_array_append(out, (const uint8_t *)"\x89PNG\r\n\x1a\n", 8);
    stl_be_p(ihdr, width);
    stl_be_p(ihdr + 4, height);
    ihdr[8] = 8;    /* bit depth */
    ihdr[9] = 2;    /* colour type: RGB */
    ihdr[10] = 0;   /* compression: deflate */
    ihdr[11] = 0;   /* filter method 0 */
    ihdr[12] = 0;   /* no interlace */
    png_put_chunk(out, "IHDR", ihdr, sizeof(ihdr));
    png_put_chunk(out, "IDAT", idat->data, idat->len);
    png_put_chunk(out, "IEND", NULL, 0);
    return out;

fail:
    deflateEnd(&zs);
    error_setg(errp, "PNG: deflate failed");
    return NULL;
}
