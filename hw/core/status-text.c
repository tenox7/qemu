/*
 * Status text: a device's status code as text
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/status-text.h"
#include "qemu/module.h"

typedef struct StatusTextWalk {
    void (*fn)(Object *obj, const char *text, void *opaque);
    void *opaque;
} StatusTextWalk;

static int status_text_one(Object *obj, void *opaque)
{
    StatusTextWalk *walk = opaque;
    StatusTextClass *stc;
    g_autofree char *text = NULL;

    if (!object_dynamic_cast(obj, TYPE_STATUS_TEXT)) {
        return 0;
    }
    stc = STATUS_TEXT_GET_CLASS(obj);
    text = stc->get_status_text ? stc->get_status_text(obj) : NULL;
    if (text) {
        walk->fn(obj, text, walk->opaque);
    }
    return 0;
}

void status_text_foreach(void (*fn)(Object *obj, const char *text,
                                    void *opaque),
                         void *opaque)
{
    StatusTextWalk walk = { fn, opaque };

    object_child_foreach_recursive(object_get_root(), status_text_one,
                                   &walk);
}

static const TypeInfo status_text_info = {
    .name          = TYPE_STATUS_TEXT,
    .parent        = TYPE_INTERFACE,
    .class_size    = sizeof(StatusTextClass),
};

static void status_text_register_types(void)
{
    type_register_static(&status_text_info);
}

type_init(status_text_register_types)
