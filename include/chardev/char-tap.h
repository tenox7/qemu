/*
 * Character device taps: a history of what a frontend wrote
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CHAR_TAP_H
#define CHAR_TAP_H

#include "chardev/char-fe.h"
#include "qemu/notify.h"

/*
 * A tap keeps the last @size bytes that a frontend (a guest serial
 * port) wrote.  Offsets are absolute: they count every byte since the
 * tap was made, so a reader can keep a cursor and tell when it fell
 * behind.  New data runs the tap's notifiers from the main loop.
 */
typedef struct CharTap CharTap;

CharTap *qemu_chr_tap_new(const char *name, size_t size);
const char *qemu_chr_tap_name(CharTap *t);
/* Tap @c's output; NULL stops it */
void qemu_chr_fe_set_tap(CharFrontend *c, CharTap *t);
void qemu_chr_tap_append(CharTap *t, const uint8_t *buf, size_t len);
/* Offsets of the oldest kept byte and of the end */
void qemu_chr_tap_range(CharTap *t, uint64_t *start, uint64_t *end);
/*
 * Copy up to @max bytes from @from.  If @from is older than the oldest
 * kept byte, reading starts there; *@actual says where.
 */
size_t qemu_chr_tap_read(CharTap *t, uint64_t from, uint8_t *buf,
                         size_t max, uint64_t *actual);
void qemu_chr_tap_add_notifier(CharTap *t, Notifier *n);
void qemu_chr_tap_remove_notifier(Notifier *n);

#endif
