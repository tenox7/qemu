/*
 * PNG encoder that needs only zlib
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef UI_PNG_H
#define UI_PNG_H

#include "ui/qemu-pixman.h"

/*
 * Encode @image as an 8-bit RGB PNG.  Rows are converted and
 * compressed one at a time, so no raw copy of the image is made.
 * Returns the PNG file contents, or NULL with @errp set.
 */
GByteArray *qemu_png_encode(pixman_image_t *image, Error **errp);

#endif
