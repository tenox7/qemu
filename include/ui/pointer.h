/*
 * Pointer model: put the guest's mouse pointer on a given pixel
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef UI_POINTER_H
#define UI_POINTER_H

#include "ui/console.h"
#include "ui/input.h"

typedef enum QemuPointerMethod {
    QEMU_POINTER_AUTO,
    QEMU_POINTER_ABSOLUTE,      /* an absolute device: exact */
    QEMU_POINTER_CLOSED_LOOP,   /* relative, steered by the card's cursor */
    QEMU_POINTER_OPEN_LOOP,     /* relative, homed, small paced steps */
} QemuPointerMethod;

typedef enum QemuPointerHome {
    QEMU_POINTER_HOME_AUTO,     /* when the position is unknown or old */
    QEMU_POINTER_HOME_ALWAYS,
    QEMU_POINTER_HOME_NEVER,
} QemuPointerHome;

typedef struct QemuPointerOpts {
    QemuPointerMethod method;
    QemuPointerHome home;
    int step_px;                /* open loop: largest step */
    bool step_auto;             /* step_px is the default: calibrate */
    int step_delay_ms;          /* open loop: time between steps */
} QemuPointerOpts;

typedef struct QemuPointerResult {
    QemuPointerMethod method;
    bool homed;
    int steps;
    int step_px;                /* open loop: the largest step it used */
    bool calibrated;            /* the step was measured in this call */
    bool known;
    int x, y;
} QemuPointerResult;

void qemu_pointer_opts_init(QemuPointerOpts *o);
const char *qemu_pointer_method_str(QemuPointerMethod m);

/* Move the pointer of @con to @x, @y (framebuffer pixels) */
bool coroutine_fn qemu_pointer_co_move(QemuConsole *con, QemuInputCtx *ctx,
                                       int x, int y,
                                       const QemuPointerOpts *o,
                                       QemuPointerResult *res, Error **errp);
/* Move by @dx, @dy in small steps; the position stays known if it was */
bool coroutine_fn qemu_pointer_co_move_by(QemuConsole *con, QemuInputCtx *ctx,
                                          int dx, int dy,
                                          const QemuPointerOpts *o,
                                          QemuPointerResult *res,
                                          Error **errp);
/*
 * Find where the pointer really is and re-sync the model: from the
 * card's cursor if it has one, else by nudging the mouse and looking
 * for what moved.  *@how says which, for the user.
 */
bool coroutine_fn qemu_pointer_co_locate(QemuConsole *con, QemuInputCtx *ctx,
                                         QemuPointerResult *res,
                                         const char **how, Error **errp);
/* Where lemu thinks the pointer is */
bool qemu_pointer_get(QemuConsole *con, int *x, int *y,
                      QemuPointerMethod *method);
/* Forget the position, for example after a mode change */
void qemu_pointer_forget(QemuConsole *con);

#endif
