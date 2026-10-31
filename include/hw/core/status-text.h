/*
 * Status text: a device's status code as text
 *
 * Some devices show a status on the machine itself, such as a front
 * panel LED display.  Through this interface they report it as text,
 * so an agent can read it from vm-info instead of from a picture.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_STATUS_TEXT_H
#define HW_STATUS_TEXT_H

#include "qom/object.h"

#define TYPE_STATUS_TEXT "status-text"

typedef struct StatusTextClass StatusTextClass;
DECLARE_CLASS_CHECKERS(StatusTextClass, STATUS_TEXT, TYPE_STATUS_TEXT)

struct StatusTextClass {
    InterfaceClass parent_class;

    /*
     * get_status_text: the status now, for example "0c31".  The caller
     * frees it.  NULL means there is nothing to show.
     */
    char *(*get_status_text)(Object *obj);
};

/* Call @fn for each object that has status text, with the text */
void status_text_foreach(void (*fn)(Object *obj, const char *text,
                                    void *opaque),
                         void *opaque);

#endif
