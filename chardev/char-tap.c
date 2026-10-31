/*
 * Character device taps: a history of what a frontend wrote
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "chardev/char-tap.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"

struct CharTap {
    char *name;
    QemuMutex lock;
    uint8_t *ring;
    size_t size;
    uint64_t end;               /* absolute offset after the last byte */
    QEMUBH *bh;                 /* runs the notifiers */
    NotifierList notifiers;
};

static void qemu_chr_tap_bh(void *opaque)
{
    CharTap *t = opaque;

    notifier_list_notify(&t->notifiers, t);
}

CharTap *qemu_chr_tap_new(const char *name, size_t size)
{
    CharTap *t = g_new0(CharTap, 1);

    t->name = g_strdup(name);
    qemu_mutex_init(&t->lock);
    t->ring = g_malloc(size);
    t->size = size;
    t->bh = qemu_bh_new(qemu_chr_tap_bh, t);
    notifier_list_init(&t->notifiers);
    return t;
}

const char *qemu_chr_tap_name(CharTap *t)
{
    return t->name;
}

void qemu_chr_fe_set_tap(CharFrontend *c, CharTap *t)
{
    c->tap = t;
}

/* May run in a vCPU thread; the notifiers run later in the main loop */
void qemu_chr_tap_append(CharTap *t, const uint8_t *buf, size_t len)
{
    WITH_QEMU_LOCK_GUARD(&t->lock) {
        if (len > t->size) {
            t->end += len - t->size;
            buf += len - t->size;
            len = t->size;
        }
        while (len) {
            size_t pos = t->end % t->size;
            size_t n = MIN(len, t->size - pos);

            memcpy(t->ring + pos, buf, n);
            t->end += n;
            buf += n;
            len -= n;
        }
    }
    qemu_bh_schedule(t->bh);
}

void qemu_chr_tap_range(CharTap *t, uint64_t *start, uint64_t *end)
{
    QEMU_LOCK_GUARD(&t->lock);
    *end = t->end;
    *start = t->end > t->size ? t->end - t->size : 0;
}

size_t qemu_chr_tap_read(CharTap *t, uint64_t from, uint8_t *buf,
                         size_t max, uint64_t *actual)
{
    uint64_t start;
    size_t done = 0;

    QEMU_LOCK_GUARD(&t->lock);
    start = t->end > t->size ? t->end - t->size : 0;
    from = MIN(MAX(from, start), t->end);
    *actual = from;
    while (done < max && from < t->end) {
        size_t pos = from % t->size;
        size_t n = MIN(MIN(max - done, t->end - from), t->size - pos);

        memcpy(buf + done, t->ring + pos, n);
        done += n;
        from += n;
    }
    return done;
}

void qemu_chr_tap_add_notifier(CharTap *t, Notifier *n)
{
    notifier_list_add(&t->notifiers, n);
}

void qemu_chr_tap_remove_notifier(Notifier *n)
{
    notifier_remove(n);
}
