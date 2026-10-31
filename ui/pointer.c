/*
 * Pointer model: put the guest's mouse pointer on a given pixel
 *
 * Old guests have relative mice with acceleration, so "move to x,y"
 * has no direct meaning there.  Three ways to get there anyway:
 *
 * - absolute: the device takes absolute positions.  Exact.
 * - closed loop: the display card reports where it draws its hardware
 *   cursor.  Send a move, read the cursor back, correct, repeat.
 *   Acceleration does not matter.
 * - open loop: push the pointer into the top-left corner, where guests
 *   clamp it, then step to the target in moves small enough to stay
 *   below guest acceleration, paced so the mouse device does not merge
 *   them into bigger moves.  Some guests speed up any move bigger than
 *   a pixel, so the first open loop move measures how big a step the
 *   guest takes as it is.
 *
 * The model forgets the position when something else moves the mouse,
 * when the screen size changes, and on reset.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/queue.h"
#include "qemu/timer.h"
#include "system/reset.h"
#include "ui/frames.h"
#include "ui/pointer.h"
#include "trace.h"

#define POINTER_HOME_PACKET     100     /* px per homing packet */
#define POINTER_STALE_MS        10000
#define POINTER_LOOP_CAP        40      /* largest closed-loop move */
#define POINTER_LOOP_MAX        300     /* closed-loop iterations */
#define POINTER_POLL_MS         10
#define POINTER_SETTLE_MS       150
#define POINTER_CAL_DIST        48      /* px each calibration move covers */
#define POINTER_CAL_SLACK       2       /* px two landings may differ */

typedef struct QemuPointer {
    QemuConsole *con;
    bool known;
    int x, y;
    int64_t sync_ms;
    int fb_w, fb_h;
    uint64_t foreign;
    QemuPointerMethod method;
    /* closed loop: hotspot minus the cursor image's top-left corner */
    bool has_offset;
    int off_x, off_y;
    /* open loop: the biggest step the guest takes as it is; 0: unknown */
    bool cal_done;
    int cal_step;
    int cal_w, cal_h;           /* the screen size it was measured at */
    QLIST_ENTRY(QemuPointer) node;
} QemuPointer;

static QLIST_HEAD(, QemuPointer) pointers = QLIST_HEAD_INITIALIZER(pointers);

static void pointer_reset(void *opaque)
{
    QemuPointer *p;

    QLIST_FOREACH(p, &pointers, node) {
        p->known = false;
        p->has_offset = false;
        p->cal_done = false;    /* another guest may boot */
    }
}

static QemuPointer *pointer_find(QemuConsole *con, bool create)
{
    QemuPointer *p;

    QLIST_FOREACH(p, &pointers, node) {
        if (p->con == con) {
            return p;
        }
    }
    if (!create) {
        return NULL;
    }
    if (QLIST_EMPTY(&pointers)) {
        qemu_register_reset(pointer_reset, NULL);
    }
    p = g_new0(QemuPointer, 1);
    p->con = con;
    QLIST_INSERT_HEAD(&pointers, p, node);
    return p;
}

static bool pointer_valid(QemuPointer *p, int w, int h)
{
    return p->known && p->fb_w == w && p->fb_h == h &&
           p->foreign == qemu_input_foreign_mouse_count();
}

static void pointer_set(QemuPointer *p, int x, int y, int w, int h,
                        QemuPointerMethod method)
{
    p->known = true;
    p->x = x;
    p->y = y;
    p->fb_w = w;
    p->fb_h = h;
    p->method = method;
    p->sync_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    p->foreign = qemu_input_foreign_mouse_count();
}

void qemu_pointer_opts_init(QemuPointerOpts *o)
{
    o->method = QEMU_POINTER_AUTO;
    o->home = QEMU_POINTER_HOME_AUTO;
    o->step_px = 4;
    o->step_auto = true;
    o->step_delay_ms = 20;
}

const char *qemu_pointer_method_str(QemuPointerMethod m)
{
    switch (m) {
    case QEMU_POINTER_ABSOLUTE:
        return "absolute";
    case QEMU_POINTER_CLOSED_LOOP:
        return "closed-loop";
    case QEMU_POINTER_OPEN_LOOP:
        return "open-loop";
    default:
        return "auto";
    }
}

bool qemu_pointer_get(QemuConsole *con, int *x, int *y,
                      QemuPointerMethod *method)
{
    QemuPointer *p = pointer_find(con, false);

    if (!p || !pointer_valid(p, qemu_console_get_width(con, 0),
                             qemu_console_get_height(con, 0))) {
        return false;
    }
    *x = p->x;
    *y = p->y;
    *method = p->method;
    return true;
}

void qemu_pointer_forget(QemuConsole *con)
{
    QemuPointer *p = pointer_find(con, false);

    if (p) {
        p->known = false;
        p->has_offset = false;
    }
}

static int clamp_step(int d, int cap)
{
    return MAX(-cap, MIN(cap, d));
}

static void pointer_add_homing(QemuInputBatch *b, int w, int h,
                               int dir, int delay_ms)
{
    int n = DIV_ROUND_UP(2 * MAX(w, h), POINTER_HOME_PACKET);
    int i;

    for (i = 0; i < n; i++) {
        qemu_input_batch_move(b, INPUT_EVENT_KIND_REL,
                              dir * POINTER_HOME_PACKET,
                              dir * POINTER_HOME_PACKET);
        qemu_input_batch_delay(b, delay_ms);
    }
}

static bool coroutine_fn pointer_co_absolute(QemuPointer *p,
                                             QemuInputCtx *ctx, int x, int y,
                                             int w, int h,
                                             QemuPointerResult *res,
                                             Error **errp)
{
    QemuInputBatch *b = qemu_input_batch_new(p->con);

    qemu_input_batch_move(b, INPUT_EVENT_KIND_ABS,
                          qemu_input_scale_axis(x, 0, MAX(w - 1, 1),
                                                INPUT_EVENT_ABS_MIN,
                                                INPUT_EVENT_ABS_MAX),
                          qemu_input_scale_axis(y, 0, MAX(h - 1, 1),
                                                INPUT_EVENT_ABS_MIN,
                                                INPUT_EVENT_ABS_MAX));
    if (!qemu_input_co_run(ctx, b, errp)) {
        return false;
    }
    pointer_set(p, x, y, w, h, QEMU_POINTER_ABSOLUTE);
    res->method = QEMU_POINTER_ABSOLUTE;
    res->steps = 1;
    return true;
}

#define POINTER_WIGGLE          12      /* px for wiggle-and-diff */
#define POINTER_WIGGLE_MAX_BOX  128     /* a bigger change is not the pointer */

/* Bounding box of the pixels that differ between @a and @b */
static bool pointer_diff_box(pixman_image_t *a, pixman_image_t *b,
                             int *x0, int *y0, int *x1, int *y1)
{
    int w = pixman_image_get_width(a), h = pixman_image_get_height(a);
    int sa = pixman_image_get_stride(a) / 4;
    int sb = pixman_image_get_stride(b) / 4;
    uint32_t *da = pixman_image_get_data(a), *db = pixman_image_get_data(b);
    int x, y;

    *x0 = w;
    *y0 = h;
    *x1 = *y1 = -1;
    if (w != pixman_image_get_width(b) || h != pixman_image_get_height(b)) {
        return false;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            if ((da[y * sa + x] ^ db[y * sb + x]) & 0xffffff) {
                *x0 = MIN(*x0, x);
                *y0 = MIN(*y0, y);
                *x1 = MAX(*x1, x);
                *y1 = MAX(*y1, y);
            }
        }
    }
    return *x1 >= 0;
}

static bool coroutine_fn pointer_co_nudge(QemuInputCtx *ctx, QemuConsole *con,
                                          int dx, int dy, Error **errp)
{
    QemuInputBatch *b = qemu_input_batch_new(con);

    qemu_input_batch_move(b, INPUT_EVENT_KIND_REL, dx, dy);
    qemu_input_batch_delay(b, POINTER_SETTLE_MS);
    return qemu_input_co_run(ctx, b, errp);
}

/*
 * Find the pointer without the card's cursor: from the display's cursor
 * if it reports one, else nudge the mouse and look at what moved; the
 * old pointer image's top-left corner is the top-left of the change.
 */
static bool coroutine_fn pointer_co_find(QemuConsole *con, QemuInputCtx *ctx,
                                         int *x, int *y, const char **how,
                                         Error **errp)
{
    g_autoptr(pixman_image_t) before = NULL;
    g_autoptr(pixman_image_t) after = NULL;
    int x0, y0, x1, y1;
    bool on;

    if (qemu_frames_get_hw_cursor(qemu_console_frames(con), x, y, &on) &&
        on) {
        *how = "display cursor";
        return true;
    }
    before = qemu_console_co_capture(con, NULL, 1.0, errp);
    if (!before ||
        !pointer_co_nudge(ctx, con, POINTER_WIGGLE, POINTER_WIGGLE, errp)) {
        return false;
    }
    after = qemu_console_co_capture(con, NULL, 1.0, errp);
    if (!after ||
        !pointer_co_nudge(ctx, con, -POINTER_WIGGLE, -POINTER_WIGGLE, errp)) {
        return false;
    }
    if (!pointer_diff_box(before, after, &x0, &y0, &x1, &y1)) {
        error_setg(errp, "nothing on the screen moved with the mouse; the "
                   "guest may draw no pointer here");
        return false;
    }
    if (x1 - x0 > POINTER_WIGGLE_MAX_BOX || y1 - y0 > POINTER_WIGGLE_MAX_BOX) {
        error_setg(errp, "the screen changed in %d,%d %dx%d while the mouse "
                   "moved, too much to be the pointer; try again when it is "
                   "still", x0, y0, x1 - x0 + 1, y1 - y0 + 1);
        return false;
    }
    *x = x0;
    *y = y0;
    *how = "screen diff, pointer image corner (approximate)";
    return true;
}

static bool pointer_cal_valid(QemuPointer *p, int w, int h)
{
    return p->cal_done && p->cal_w == w && p->cal_h == h;
}

/* Home, then step POINTER_CAL_DIST px down and right; where did it land? */
static bool coroutine_fn pointer_co_cal_probe(QemuPointer *p,
                                              QemuInputCtx *ctx,
                                              int w, int h, int step,
                                              const QemuPointerOpts *o,
                                              int *x, int *y, Error **errp)
{
    QemuInputBatch *b = qemu_input_batch_new(p->con);
    const char *how;
    int i;

    pointer_add_homing(b, w, h, -1, o->step_delay_ms);
    for (i = 0; i < POINTER_CAL_DIST / step; i++) {
        qemu_input_batch_move(b, INPUT_EVENT_KIND_REL, step, step);
        qemu_input_batch_delay(b, o->step_delay_ms);
    }
    qemu_input_batch_delay(b, POINTER_SETTLE_MS);
    return qemu_input_co_run(ctx, b, errp) &&
           pointer_co_find(p->con, ctx, x, y, &how, errp);
}

/*
 * Find the biggest step that lands where 1-pixel steps land, so the
 * guest does not speed it up.  Both landings are found the same way, so
 * the hotspot of the pointer image cancels out.  If the pointer cannot
 * be found, the default step stays.  False only if cancelled.
 */
static bool coroutine_fn pointer_co_calibrate(QemuPointer *p,
                                              QemuInputCtx *ctx,
                                              int w, int h,
                                              const QemuPointerOpts *o,
                                              Error **errp)
{
    static const int steps[] = { 4, 2 };
    Error *local_err = NULL;
    int rx, ry, x, y, i;

    p->cal_done = true;
    p->cal_w = w;
    p->cal_h = h;
    p->cal_step = 0;
    if (!pointer_co_cal_probe(p, ctx, w, h, 1, o, &rx, &ry, &local_err)) {
        goto out;
    }
    for (i = 0; i < ARRAY_SIZE(steps); i++) {
        if (steps[i] > MAX(1, o->step_px)) {
            continue;
        }
        if (!pointer_co_cal_probe(p, ctx, w, h, steps[i], o, &x, &y,
                                  &local_err)) {
            goto out;
        }
        if (abs(x - rx) <= POINTER_CAL_SLACK &&
            abs(y - ry) <= POINTER_CAL_SLACK) {
            p->cal_step = steps[i];
            break;
        }
    }
    if (!p->cal_step) {
        p->cal_step = 1;
    }

out:
    trace_pointer_calibrate(qemu_console_get_index(p->con), p->cal_step,
                            local_err ? error_get_pretty(local_err) : "");
    if (local_err && ctx->cancelled) {
        error_propagate(errp, local_err);
        return false;
    }
    error_free(local_err);
    return true;
}

/* Step from the model's position (homing first if needed) to @x, @y */
static bool coroutine_fn pointer_co_open_loop(QemuPointer *p,
                                              QemuInputCtx *ctx,
                                              int x, int y, int w, int h,
                                              const QemuPointerOpts *o,
                                              QemuPointerResult *res,
                                              Error **errp)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    bool valid = pointer_valid(p, w, h);
    int step = MAX(1, o->step_px);
    QemuInputBatch *b;
    int cx, cy;

    switch (o->home) {
    case QEMU_POINTER_HOME_ALWAYS:
        res->homed = true;
        break;
    case QEMU_POINTER_HOME_NEVER:
        if (!valid) {
            error_setg(errp, "pointer position is unknown; move with home "
                       "set to auto or always");
            return false;
        }
        break;
    default:
        res->homed = !valid || now - p->sync_ms > POINTER_STALE_MS;
        break;
    }

    if (o->step_auto && o->home != QEMU_POINTER_HOME_NEVER &&
        !pointer_cal_valid(p, w, h)) {
        if (!pointer_co_calibrate(p, ctx, w, h, o, errp)) {
            p->known = false;
            return false;
        }
        res->calibrated = true;
        res->homed = true;      /* it left the pointer somewhere else */
    }
    if (o->step_auto && pointer_cal_valid(p, w, h) && p->cal_step) {
        step = MIN(step, p->cal_step);
    }
    res->step_px = step;

    b = qemu_input_batch_new(p->con);
    if (res->homed) {
        pointer_add_homing(b, w, h, -1, o->step_delay_ms);
        cx = cy = 0;
    } else {
        cx = p->x;
        cy = p->y;
    }
    while (cx != x || cy != y) {
        int sx = clamp_step(x - cx, step);
        int sy = clamp_step(y - cy, step);

        qemu_input_batch_move(b, INPUT_EVENT_KIND_REL, sx, sy);
        qemu_input_batch_delay(b, o->step_delay_ms);
        cx += sx;
        cy += sy;
        res->steps++;
    }
    if (!qemu_input_co_run(ctx, b, errp)) {
        p->known = false;
        return false;
    }
    pointer_set(p, x, y, w, h, QEMU_POINTER_OPEN_LOOP);
    res->method = QEMU_POINTER_OPEN_LOOP;
    return true;
}

static bool cursor_equal(const QemuCursorState *a, const QemuCursorState *b)
{
    return a->x == b->x && a->y == b->y && a->visible == b->visible;
}

/* Sleep until the card's cursor moves away from @old and settles */
static bool coroutine_fn pointer_co_await_cursor(QemuPointer *p,
                                                 QemuInputCtx *ctx,
                                                 const QemuCursorState *old,
                                                 QemuCursorState *cs)
{
    bool moved = false;
    int waited;

    for (waited = 0; waited < POINTER_SETTLE_MS; waited += POINTER_POLL_MS) {
        QemuCursorState prev;

        qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME,
                                  POINTER_POLL_MS * SCALE_MS);
        if (ctx->cancelled) {
            return false;
        }
        prev = *cs;
        qemu_console_hw_cursor_state(p->con, cs);
        if (moved && cursor_equal(&prev, cs)) {
            break;          /* moved, then held still for one poll */
        }
        if (!cursor_equal(old, cs)) {
            moved = true;
        }
    }
    return moved;
}

static bool coroutine_fn pointer_co_closed_loop(QemuPointer *p,
                                                QemuInputCtx *ctx,
                                                int x, int y, int w, int h,
                                                const QemuPointerOpts *o,
                                                QemuPointerResult *res,
                                                Error **errp)
{
    int capx = POINTER_LOOP_CAP, capy = POINTER_LOOP_CAP;
    int stuck = 0;
    QemuCursorState cs;
    int i;

    qemu_console_hw_cursor_state(p->con, &cs);

    if (!p->has_offset || o->home == QEMU_POINTER_HOME_ALWAYS) {
        /*
         * Learn the hotspot: guests clamp the hotspot to the last
         * pixel, so in the bottom-right corner the offset shows.
         */
        QemuInputBatch *b = qemu_input_batch_new(p->con);
        QemuCursorState old = cs;

        pointer_add_homing(b, w, h, 1, o->step_delay_ms);
        if (!qemu_input_co_run(ctx, b, errp)) {
            return false;
        }
        pointer_co_await_cursor(p, ctx, &old, &cs);
        if (ctx->cancelled) {
            error_setg(errp, "cancelled");
            return false;
        }
        p->off_x = (w - 1) - cs.x;
        p->off_y = (h - 1) - cs.y;
        if (p->off_x < 0 || p->off_x >= MAX(cs.w, 1) ||
            p->off_y < 0 || p->off_y >= MAX(cs.h, 1)) {
            p->off_x = p->off_y = 0;
        }
        p->has_offset = true;
        res->homed = true;
    }

    for (i = 0; i < POINTER_LOOP_MAX; i++) {
        int dx = x - (cs.x + p->off_x);
        int dy = y - (cs.y + p->off_y);
        QemuCursorState old = cs;
        QemuInputBatch *b;
        int ndx, ndy;

        if (!dx && !dy) {
            pointer_set(p, x, y, w, h, QEMU_POINTER_CLOSED_LOOP);
            res->method = QEMU_POINTER_CLOSED_LOOP;
            return true;
        }
        b = qemu_input_batch_new(p->con);
        qemu_input_batch_move(b, INPUT_EVENT_KIND_REL,
                              clamp_step(dx, capx), clamp_step(dy, capy));
        if (!qemu_input_co_run(ctx, b, errp)) {
            p->known = false;
            return false;
        }
        res->steps++;
        if (!pointer_co_await_cursor(p, ctx, &old, &cs)) {
            if (ctx->cancelled) {
                error_setg(errp, "cancelled");
                return false;
            }
            if (++stuck >= 3) {
                break;
            }
            continue;
        }
        stuck = 0;
        /* overshot: the guest accelerated the move, so move less */
        ndx = x - (cs.x + p->off_x);
        ndy = y - (cs.y + p->off_y);
        if (ndx && (ndx > 0) != (dx > 0)) {
            capx = MAX(1, capx / 2);
        }
        if (ndy && (ndy > 0) != (dy > 0)) {
            capy = MAX(1, capy / 2);
        }
    }

    /* the card's cursor stopped following: finish in open loop from here */
    pointer_set(p, cs.x + p->off_x, cs.y + p->off_y, w, h,
                QEMU_POINTER_OPEN_LOOP);
    {
        QemuPointerOpts oo = *o;

        oo.home = QEMU_POINTER_HOME_NEVER;
        return pointer_co_open_loop(p, ctx, x, y, w, h, &oo, res, errp);
    }
}

bool coroutine_fn qemu_pointer_co_move(QemuConsole *con, QemuInputCtx *ctx,
                                       int x, int y,
                                       const QemuPointerOpts *o,
                                       QemuPointerResult *res, Error **errp)
{
    QemuPointer *p = pointer_find(con, true);
    int w = qemu_console_get_width(con, 0);
    int h = qemu_console_get_height(con, 0);
    QemuPointerMethod m = o->method;
    QemuCursorState cs;
    bool ok;

    memset(res, 0, sizeof(*res));
    if (w <= 0 || h <= 0) {
        error_setg(errp, "the console has no screen");
        return false;
    }
    if (x < 0 || y < 0 || x >= w || y >= h) {
        error_setg(errp, "%d,%d is outside the %dx%d screen", x, y, w, h);
        return false;
    }

    if (m == QEMU_POINTER_AUTO) {
        if (qemu_input_is_absolute(con)) {
            m = QEMU_POINTER_ABSOLUTE;
        } else if (qemu_console_hw_cursor_state(con, &cs) && cs.visible) {
            m = QEMU_POINTER_CLOSED_LOOP;
        } else {
            m = QEMU_POINTER_OPEN_LOOP;
        }
    }

    switch (m) {
    case QEMU_POINTER_ABSOLUTE:
        if (!qemu_input_is_absolute(con)) {
            error_setg(errp, "the mouse of this console is relative");
            return false;
        }
        ok = pointer_co_absolute(p, ctx, x, y, w, h, res, errp);
        break;
    case QEMU_POINTER_CLOSED_LOOP:
        if (!qemu_console_hw_cursor_state(con, &cs) || !cs.visible) {
            error_setg(errp, "the display card shows no hardware cursor");
            return false;
        }
        ok = pointer_co_closed_loop(p, ctx, x, y, w, h, o, res, errp);
        break;
    default:
        ok = pointer_co_open_loop(p, ctx, x, y, w, h, o, res, errp);
        break;
    }

    res->known = p->known;
    res->x = p->x;
    res->y = p->y;
    trace_pointer_move(qemu_console_get_index(con), x, y,
                       qemu_pointer_method_str(res->method), res->homed,
                       res->steps, ok);
    return ok;
}

bool coroutine_fn qemu_pointer_co_move_by(QemuConsole *con, QemuInputCtx *ctx,
                                          int dx, int dy,
                                          const QemuPointerOpts *o,
                                          QemuPointerResult *res,
                                          Error **errp)
{
    QemuPointer *p = pointer_find(con, true);
    int w = qemu_console_get_width(con, 0);
    int h = qemu_console_get_height(con, 0);
    int step = MAX(1, o->step_px);
    QemuInputBatch *b;

    if (pointer_valid(p, w, h)) {
        QemuPointerOpts oo = *o;

        oo.home = QEMU_POINTER_HOME_NEVER;
        return qemu_pointer_co_move(con, ctx,
                                    MAX(0, MIN(w - 1, p->x + dx)),
                                    MAX(0, MIN(h - 1, p->y + dy)),
                                    &oo, res, errp);
    }

    /* position unknown: just send the steps */
    memset(res, 0, sizeof(*res));
    if (o->step_auto && pointer_cal_valid(p, w, h) && p->cal_step) {
        step = MIN(step, p->cal_step);
    }
    res->step_px = step;
    b = qemu_input_batch_new(con);
    while (dx || dy) {
        int sx = clamp_step(dx, step);
        int sy = clamp_step(dy, step);

        qemu_input_batch_move(b, INPUT_EVENT_KIND_REL, sx, sy);
        qemu_input_batch_delay(b, o->step_delay_ms);
        dx -= sx;
        dy -= sy;
        res->steps++;
    }
    res->method = QEMU_POINTER_OPEN_LOOP;
    return qemu_input_co_run(ctx, b, errp);
}

bool coroutine_fn qemu_pointer_co_locate(QemuConsole *con, QemuInputCtx *ctx,
                                         QemuPointerResult *res,
                                         const char **how, Error **errp)
{
    int w = qemu_console_get_width(con, 0);
    int h = qemu_console_get_height(con, 0);
    QemuPointer *p = pointer_find(con, true);
    QemuCursorState cs;
    int x, y;

    memset(res, 0, sizeof(*res));
    if (qemu_input_is_absolute(con)) {
        if (!pointer_valid(p, w, h)) {
            error_setg(errp, "the mouse is absolute, so it is wherever "
                       "it was last sent; use mouse-move to place it");
            return false;
        }
        *how = "absolute device, last position sent";
        goto found_model;
    }
    if (qemu_console_hw_cursor_state(con, &cs) && cs.visible) {
        if (p->has_offset) {
            pointer_set(p, cs.x + p->off_x, cs.y + p->off_y, w, h,
                        QEMU_POINTER_CLOSED_LOOP);
            *how = "display card cursor";
        } else {
            /* without the learned offset, the image corner is the guess */
            pointer_set(p, cs.x, cs.y, w, h, QEMU_POINTER_CLOSED_LOOP);
            *how = "display card cursor, image corner (approximate)";
        }
        goto found_model;
    }
    if (!pointer_co_find(con, ctx, &x, &y, how, errp)) {
        return false;
    }
    pointer_set(p, x, y, w, h, QEMU_POINTER_OPEN_LOOP);

found_model:
    res->known = true;
    res->x = p->x;
    res->y = p->y;
    res->method = p->method;
    trace_pointer_move(qemu_console_get_index(con), p->x, p->y, *how,
                       false, 0, true);
    return true;
}
