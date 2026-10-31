/*
 * Frame tracking and screen waits
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef UI_FRAMES_H
#define UI_FRAMES_H

#include "qemu/coroutine.h"
#include "ui/console.h"

typedef struct QemuFrames QemuFrames;

typedef struct QemuFramesRect {
    int x, y, w, h;
} QemuFramesRect;

typedef enum QemuFramesUntil {
    QEMU_FRAMES_CHANGE,
    QEMU_FRAMES_STABLE,
    QEMU_FRAMES_CHANGE_THEN_STABLE,
    QEMU_FRAMES_PIXEL,
} QemuFramesUntil;

typedef struct QemuFramesCond {
    QemuFramesUntil until;
    bool has_region;
    QemuFramesRect region;      /* only changes in here count */
    const QemuFramesRect *ignore;
    int n_ignore;               /* changes in these do not count */
    int min_pixels;             /* a change needs this many pixels */
    int stable_ms;
    int timeout_ms;
    int pixel_x, pixel_y;
    uint32_t pixel_rgb;         /* 0xRRGGBB */
    int pixel_tolerance;        /* per channel */
    bool ignore_blink;          /* skip blinking text cursors */
    bool ignore_animation;      /* skip areas that keep changing */
    /* the caller's sleep; waking it with @cancelled set ends the wait */
    QemuCoSleep *sleep;
    const bool *cancelled;
} QemuFramesCond;

#define QEMU_FRAMES_ANIM_MAX    8

typedef struct QemuFramesResult {
    bool matched;
    bool cancelled;
    int64_t elapsed_ms;
    int64_t changed_after_ms;   /* first counted change; -1 if none */
    int64_t stable_after_ms;    /* last counted change; -1 if none */
    int changed_pixels;
    bool has_box;
    QemuFramesRect box;         /* union of the counted changes */
    uint64_t frame;
    /* areas that kept changing on their own during the wait */
    int n_animations;
    QemuFramesRect animations[QEMU_FRAMES_ANIM_MAX];
} QemuFramesResult;

/*
 * The frame tracker of @con, created on first use.  It watches the
 * console through a display listener, so frames are rendered even with
 * -display none.  The frame number grows only when pixels change.
 */
QemuFrames *qemu_console_frames(QemuConsole *con);
uint64_t qemu_frames_get_frame(QemuFrames *f);
/* Refresh quickly for a few seconds, for example after input */
void qemu_frames_activity(QemuFrames *f);
/* The hardware cursor reported through dpy_mouse_set, if any */
bool qemu_frames_get_hw_cursor(QemuFrames *f, int *x, int *y, bool *on);
/* Read one pixel of the last frame as 0xRRGGBB */
bool qemu_frames_get_pixel(QemuFrames *f, int x, int y, uint32_t *rgb);
/* Size of the last frame */
void qemu_frames_get_size(QemuFrames *f, int *w, int *h);
/*
 * Text mode size and cursor as the device last reported them; the size
 * is 0x0 if it never did.  Call qemu_console_hw_invalidate() and
 * qemu_console_hw_text_update() first to get them fresh.
 */
void qemu_frames_get_text(QemuFrames *f, int *w, int *h, int *cx, int *cy);

void coroutine_fn qemu_frames_co_wait(QemuFrames *f, const QemuFramesCond *c,
                                      QemuFramesResult *r);

/*
 * The same in two halves, for "act, then wait": changes count from
 * qemu_frames_wait_begin(), before the input is sent; the timeout and
 * the quiet time for "stable" count from qemu_frames_co_wait_finish().
 * Times in the result count from the begin.  @c must stay valid until
 * the finish.
 */
typedef struct QemuFramesWaiter QemuFramesWaiter;
QemuFramesWaiter *qemu_frames_wait_begin(QemuFrames *f,
                                         const QemuFramesCond *c);
void coroutine_fn qemu_frames_co_wait_finish(QemuFrames *f,
                                             QemuFramesWaiter *wt,
                                             QemuFramesResult *r);
/* Drop a wait that was begun but will not be finished */
void qemu_frames_wait_abort(QemuFrames *f, QemuFramesWaiter *wt);

/* What the history knows about a frame, newest first */
typedef struct QemuFramesInfo {
    uint64_t frame;
    int64_t ago_ms;             /* since it appeared */
    int64_t shown_ms;           /* how long it was on screen */
    bool current;
    QemuFramesRect box;         /* the change that replaced it */
    int pixels;
    int w, h;                   /* size, if a mode change replaced it */
} QemuFramesInfo;

/* Fill up to @max entries, the current frame first; blinks are left out */
int qemu_frames_history(QemuFrames *f, QemuFramesInfo *out, int max);
/* Rebuild @frame; NULL if it is too old or unknown.  Unref when done. */
pixman_image_t *qemu_frames_get_image(QemuFrames *f, uint64_t frame);
/* Track every graphic console from the start, for the history */
void qemu_frames_track_all(void);

#endif
