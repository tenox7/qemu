/*
 * Frame tracking and screen waits
 *
 * A display listener per console keeps a copy of the last frame.  On
 * every refresh it compares the dirty area with that copy, pixel by
 * pixel: some cards mark the whole screen dirty on every refresh, so
 * "dirty" is not "changed".  Only a real change bumps the frame number
 * and wakes the coroutines that wait for the screen.
 *
 * A small change that flips one box between the same two states again
 * and again is a blinking text cursor.  Waits can ignore those, so a
 * blinking cursor never keeps a "stable" wait from finishing.
 *
 * An area that keeps changing on its own, such as a clock, a graph or a
 * spinner, is an animation: about the same box changed again and again
 * for a while, never resting longer than FRAMES_ANIM_GAP_MS.  A window
 * that paints itself in a quick burst is not one.  Each wait learns them
 * from the recent history and while it waits, and then knows them until
 * it ends, also across pauses: NT's logon screen moves a hand for a
 * second every eight seconds.  A wait can ignore them, and it reports
 * them, so a wait that never settles says why.  Scrolling text looks
 * the same, so ignoring them is the caller's choice.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/queue.h"
#include "ui/frames.h"
#include "system/system.h"
#include "ui/surface.h"
#include "trace.h"

#define FRAMES_FAST_MS          30
#define FRAMES_IDLE_MS          500
#define FRAMES_ACTIVITY_MS      3000
#define FRAMES_BLINK_MAX_W      32
#define FRAMES_BLINK_MAX_H      32
#define FRAMES_BLINK_SLOTS      8
#define FRAMES_HISTORY_BYTES    (64 * 1024 * 1024)
#define FRAMES_HISTORY_MAX      4096
#define FRAMES_ANIM_CHANGES     3       /* changes in a row: an animation */
#define FRAMES_ANIM_MIN_MS      750     /* ...going on this long: not a paint */
#define FRAMES_ANIM_GAP_MS      2500    /* a longer rest while learning */
#define FRAMES_ANIM_LOOKBACK_MS 10000   /* history a wait learns from */

/*
 * History: each change keeps the old pixels of the box it changed, so
 * any recent frame can be rebuilt by undoing changes back from the
 * current one.  A mode change keeps the whole old frame.
 */
typedef struct QemuFramesUndo {
    uint64_t frame;             /* the frame these pixels belong to */
    int64_t shown_ms;           /* when that frame appeared */
    int64_t ended_ms;           /* when the change replaced it */
    QemuFramesRect box;         /* where it changed */
    int pixels;
    bool blink;
    bool whole;                 /* @old is the whole old frame */
    pixman_image_t *old;
    QTAILQ_ENTRY(QemuFramesUndo) node;
} QemuFramesUndo;

typedef struct QemuFramesAnim {
    QemuFramesRect box;
    int changes;                /* 0: free */
    int64_t first_ms, last_ms;
} QemuFramesAnim;

struct QemuFramesWaiter {
    const QemuFramesCond *cond;
    int64_t begin_ms;
    int64_t first_ms;           /* first counted change, -1 if none */
    int64_t last_ms;            /* last counted change, -1 if none */
    int pixels;
    bool has_box;
    QemuFramesRect box;
    QemuFramesAnim anim[QEMU_FRAMES_ANIM_MAX];
    QLIST_ENTRY(QemuFramesWaiter) node;
};

typedef struct QemuFramesBlink {
    QemuFramesRect box;
    uint32_t a, b;              /* hashes of the two states */
    int64_t last_ms;
} QemuFramesBlink;

struct QemuFrames {
    DisplayChangeListener dcl;
    QemuConsole *con;
    pixman_image_t *prev;       /* the last frame, x8r8g8b8 */
    int dx0, dy0, dx1, dy1;     /* dirty box; empty when dx1 <= dx0 */
    uint64_t frame;
    int64_t fast_until_ms;
    uint64_t interval;
    QLIST_HEAD(, QemuFramesWaiter) waiters;
    QemuFramesBlink blink[FRAMES_BLINK_SLOTS];
    bool cursor_valid;
    int cursor_x, cursor_y;
    bool cursor_on;
    int64_t frame_ms;           /* when the current frame appeared */
    QTAILQ_HEAD(, QemuFramesUndo) history;  /* oldest first */
    size_t history_bytes;
    int history_len;
    int text_w, text_h;         /* text mode size; 0 if never told */
    int text_cx, text_cy;       /* text cursor; -1 if hidden */
};

static int64_t frames_now_ms(void)
{
    return qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
}

static void frames_set_interval(QemuFrames *f)
{
    uint64_t interval = FRAMES_IDLE_MS;

    if (!QLIST_EMPTY(&f->waiters) || frames_now_ms() < f->fast_until_ms) {
        interval = FRAMES_FAST_MS;
    }
    if (interval != f->interval) {
        f->interval = interval;
        qemu_console_listener_set_refresh(&f->dcl, interval);
    }
}

static void frames_add_dirty(QemuFrames *f, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    if (f->dx1 <= f->dx0) {
        f->dx0 = x;
        f->dy0 = y;
        f->dx1 = x + w;
        f->dy1 = y + h;
    } else {
        f->dx0 = MIN(f->dx0, x);
        f->dy0 = MIN(f->dy0, y);
        f->dx1 = MAX(f->dx1, x + w);
        f->dy1 = MAX(f->dy1, y + h);
    }
}

static uint32_t frames_hash(pixman_image_t *img, int ox, int oy,
                            const QemuFramesRect *r)
{
    uint32_t *data = pixman_image_get_data(img);
    int stride = pixman_image_get_stride(img) / 4;
    uint32_t h = 2166136261u;
    int x, y;

    for (y = r->y; y < r->y + r->h; y++) {
        for (x = r->x; x < r->x + r->w; x++) {
            h = (h ^ (data[(y - oy) * stride + (x - ox)] & 0xffffff))
                * 16777619u;
        }
    }
    return h;
}

/* Is a change of @box from @before to @after a cursor blink? */
static bool frames_is_blink(QemuFrames *f, const QemuFramesRect *box,
                            uint32_t before, uint32_t after, int64_t now)
{
    QemuFramesBlink *e, *oldest = &f->blink[0];
    int i;

    for (i = 0; i < FRAMES_BLINK_SLOTS; i++) {
        e = &f->blink[i];
        if (e->last_ms && !memcmp(&e->box, box, sizeof(*box))) {
            bool blink = (e->a == before && e->b == after) ||
                         (e->a == after && e->b == before);
            e->a = before;
            e->b = after;
            e->last_ms = now;
            return blink;
        }
        if (e->last_ms < oldest->last_ms) {
            oldest = e;
        }
    }
    oldest->box = *box;
    oldest->a = before;
    oldest->b = after;
    oldest->last_ms = now;
    return false;
}

static bool frames_rect_contains(const QemuFramesRect *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static void frames_box_add(bool *has_box, QemuFramesRect *box,
                           int x0, int y0, int x1, int y1)
{
    if (!*has_box) {
        box->x = x0;
        box->y = y0;
        box->w = x1 - x0;
        box->h = y1 - y0;
        *has_box = true;
        return;
    }
    x1 = MAX(x1, box->x + box->w);
    y1 = MAX(y1, box->y + box->h);
    box->x = MIN(box->x, x0);
    box->y = MIN(box->y, y0);
    box->w = x1 - box->x;
    box->h = y1 - box->y;
}

static int64_t frames_rect_area(const QemuFramesRect *r)
{
    return (int64_t)r->w * r->h;
}

/* Cut @r down to @to; false if nothing is left */
static bool frames_rect_clip(QemuFramesRect *r, const QemuFramesRect *to)
{
    int x0 = MAX(r->x, to->x), y0 = MAX(r->y, to->y);
    int x1 = MIN(r->x + r->w, to->x + to->w);
    int y1 = MIN(r->y + r->h, to->y + to->h);

    if (x1 <= x0 || y1 <= y0) {
        return false;
    }
    *r = (QemuFramesRect) { x0, y0, x1 - x0, y1 - y0 };
    return true;
}

static bool frames_rect_inside(const QemuFramesRect *r,
                               const QemuFramesRect *in)
{
    return r->x >= in->x && r->y >= in->y &&
           r->x + r->w <= in->x + in->w && r->y + r->h <= in->y + in->h;
}

/* Do @a and @b cover mostly the same pixels (overlap >= half the union)? */
static bool frames_rect_same_area(const QemuFramesRect *a,
                                  const QemuFramesRect *b)
{
    QemuFramesRect overlap = *a;

    if (!frames_rect_clip(&overlap, b)) {
        return false;
    }
    return 3 * frames_rect_area(&overlap) >=
           frames_rect_area(a) + frames_rect_area(b);
}

/* An animation is too big if it covers more than a quarter of the screen */
static int64_t frames_anim_max_area(QemuFrames *f)
{
    if (!f->prev) {
        return 0;
    }
    return (int64_t)pixman_image_get_width(f->prev) *
           pixman_image_get_height(f->prev) / 4;
}

static bool frames_anim_seen(const QemuFramesAnim *a)
{
    return a->changes >= FRAMES_ANIM_CHANGES &&
           a->last_ms - a->first_ms >= FRAMES_ANIM_MIN_MS;
}

/* Learn from a change of @box at @now */
static void frames_anim_feed(QemuFramesWaiter *wt, const QemuFramesRect *box,
                             int64_t now, int64_t max_area)
{
    QemuFramesAnim *a, *oldest = &wt->anim[0];
    bool has_box = true;
    int i;

    for (i = 0; i < QEMU_FRAMES_ANIM_MAX; i++) {
        a = &wt->anim[i];
        if (a->changes && frames_rect_same_area(&a->box, box)) {
            if (!frames_anim_seen(a) &&
                now - a->last_ms > FRAMES_ANIM_GAP_MS) {
                a->changes = 0;     /* it rested: start over */
                a->box = *box;
                a->first_ms = now;
            }
            frames_box_add(&has_box, &a->box, box->x, box->y,
                           box->x + box->w, box->y + box->h);
            if (frames_rect_area(&a->box) > max_area) {
                a->changes = 0;     /* it wanders: not an animation */
                a->last_ms = 0;
                return;
            }
            a->changes++;
            a->last_ms = now;
            return;
        }
        if (a->last_ms < oldest->last_ms) {
            oldest = a;
        }
    }
    if (frames_rect_area(box) <= max_area) {
        oldest->box = *box;
        oldest->changes = 1;
        oldest->first_ms = oldest->last_ms = now;
    }
}

/*
 * Count the changed pixels (@mask covers @ox, @oy, @w x @h) that
 * @wt cares about, and record the change if there are enough.
 */
static void frames_offer_change(QemuFramesWaiter *wt, const uint8_t *mask,
                                int ox, int oy, int w, int h, bool blink,
                                int64_t now, int64_t max_area)
{
    const QemuFramesCond *c = wt->cond;
    QemuFramesRect anim[QEMU_FRAMES_ANIM_MAX];
    int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;
    int sx0 = INT_MAX, sy0 = INT_MAX, sx1 = -1, sy1 = -1;
    int count = 0, seen = 0, n_anim = 0;
    int x, y, i;

    if (blink && c->ignore_blink) {
        return;
    }
    if (c->ignore_animation) {
        for (i = 0; i < QEMU_FRAMES_ANIM_MAX; i++) {
            if (frames_anim_seen(&wt->anim[i])) {
                anim[n_anim++] = wt->anim[i].box;
            }
        }
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            bool skip = false;

            if (!mask[y * w + x]) {
                continue;
            }
            if (c->has_region &&
                !frames_rect_contains(&c->region, ox + x, oy + y)) {
                continue;
            }
            for (i = 0; i < c->n_ignore && !skip; i++) {
                skip = frames_rect_contains(&c->ignore[i], ox + x, oy + y);
            }
            if (skip) {
                continue;
            }
            seen++;
            sx0 = MIN(sx0, ox + x);
            sy0 = MIN(sy0, oy + y);
            sx1 = MAX(sx1, ox + x + 1);
            sy1 = MAX(sy1, oy + y + 1);
            for (i = 0; i < n_anim && !skip; i++) {
                skip = frames_rect_contains(&anim[i], ox + x, oy + y);
            }
            if (skip) {
                continue;
            }
            count++;
            x0 = MIN(x0, ox + x);
            y0 = MIN(y0, oy + y);
            x1 = MAX(x1, ox + x + 1);
            y1 = MAX(y1, oy + y + 1);
        }
    }
    /* learn animations from what this wait can see */
    if (!blink && seen >= MAX(1, c->min_pixels)) {
        QemuFramesRect sbox = { sx0, sy0, sx1 - sx0, sy1 - sy0 };

        frames_anim_feed(wt, &sbox, now, max_area);
    }
    if (count < MAX(1, c->min_pixels)) {
        return;
    }
    if (wt->first_ms < 0) {
        wt->first_ms = now;
    }
    wt->last_ms = now;
    wt->pixels += count;
    frames_box_add(&wt->has_box, &wt->box, x0, y0, x1, y1);
}

static void frames_history_push(QemuFrames *f, QemuFramesUndo *u)
{
    u->frame = f->frame;
    u->shown_ms = f->frame_ms;
    u->ended_ms = frames_now_ms();
    QTAILQ_INSERT_TAIL(&f->history, u, node);
    f->history_bytes += (size_t)pixman_image_get_stride(u->old) *
                        pixman_image_get_height(u->old);
    f->history_len++;
    while (f->history_len > FRAMES_HISTORY_MAX ||
           f->history_bytes > FRAMES_HISTORY_BYTES) {
        QemuFramesUndo *o = QTAILQ_FIRST(&f->history);

        QTAILQ_REMOVE(&f->history, o, node);
        f->history_bytes -= (size_t)pixman_image_get_stride(o->old) *
                            pixman_image_get_height(o->old);
        f->history_len--;
        qemu_pixman_image_unref(o->old);
        g_free(o);
    }
}

static void frames_compare(QemuFrames *f)
{
    DisplaySurface *surface = qemu_console_surface(f->con);
    g_autoptr(pixman_image_t) cur = NULL;
    g_autofree uint8_t *mask = NULL;
    QemuFramesWaiter *wt;
    uint32_t *pd, *cd;
    int pstride, cstride;
    int x0, y0, w, h, x, y;
    int bx0 = INT_MAX, by0 = INT_MAX, bx1 = -1, by1 = -1;
    int count = 0;
    bool blink = false;
    QemuFramesRect box;
    int64_t now, max_area;

    if (!surface || !f->prev || f->dx1 <= f->dx0) {
        f->dx0 = f->dx1 = 0;
        return;
    }
    x0 = MAX(f->dx0, 0);
    y0 = MAX(f->dy0, 0);
    w = MIN(f->dx1, pixman_image_get_width(f->prev)) - x0;
    h = MIN(f->dy1, pixman_image_get_height(f->prev)) - y0;
    f->dx0 = f->dx1 = 0;
    if (w <= 0 || h <= 0) {
        return;
    }

    cur = pixman_image_create_bits(PIXMAN_x8r8g8b8, w, h, NULL, 0);
    pixman_image_composite(PIXMAN_OP_SRC, surface->image, NULL, cur,
                           x0, y0, 0, 0, 0, 0, w, h);
    mask = g_malloc0((size_t)w * h);
    pd = pixman_image_get_data(f->prev);
    pstride = pixman_image_get_stride(f->prev) / 4;
    cd = pixman_image_get_data(cur);
    cstride = pixman_image_get_stride(cur) / 4;

    for (y = 0; y < h; y++) {
        uint32_t *prow = pd + (y0 + y) * pstride + x0;
        uint32_t *crow = cd + y * cstride;

        if (!memcmp(prow, crow, w * 4)) {
            continue;
        }
        for (x = 0; x < w; x++) {
            if ((prow[x] ^ crow[x]) & 0xffffff) {
                mask[y * w + x] = 1;
                count++;
                bx0 = MIN(bx0, x);
                by0 = MIN(by0, y);
                bx1 = MAX(bx1, x + 1);
                by1 = MAX(by1, y + 1);
            }
        }
    }
    if (!count) {
        return;
    }

    now = frames_now_ms();
    box = (QemuFramesRect) { x0 + bx0, y0 + by0, bx1 - bx0, by1 - by0 };
    if (box.w <= FRAMES_BLINK_MAX_W && box.h <= FRAMES_BLINK_MAX_H) {
        blink = frames_is_blink(f, &box, frames_hash(f->prev, 0, 0, &box),
                                frames_hash(cur, x0, y0, &box), now);
    }

    {
        QemuFramesUndo *u = g_new0(QemuFramesUndo, 1);

        u->box = box;
        u->pixels = count;
        u->blink = blink;
        u->old = pixman_image_create_bits(PIXMAN_x8r8g8b8, u->box.w,
                                          u->box.h, NULL, 0);
        pixman_image_composite(PIXMAN_OP_SRC, f->prev, NULL, u->old,
                               u->box.x, u->box.y, 0, 0, 0, 0,
                               u->box.w, u->box.h);
        frames_history_push(f, u);
    }
    pixman_image_composite(PIXMAN_OP_SRC, cur, NULL, f->prev,
                           0, 0, 0, 0, x0, y0, w, h);
    f->frame++;
    f->frame_ms = now;
    trace_frames_change(qemu_console_get_index(f->con), f->frame,
                        box.x, box.y, box.w, box.h, count, blink);

    max_area = frames_anim_max_area(f);
    QLIST_FOREACH(wt, &f->waiters, node) {
        frames_offer_change(wt, mask, x0, y0, w, h, blink, now, max_area);
        qemu_co_sleep_wake(wt->cond->sleep);
    }
}

static void frames_refresh(DisplayChangeListener *dcl)
{
    QemuFrames *f = container_of(dcl, QemuFrames, dcl);

    qemu_console_hw_update(f->con);
    frames_compare(f);
    frames_set_interval(f);
}

static void frames_gfx_update(DisplayChangeListener *dcl,
                              int x, int y, int w, int h)
{
    QemuFrames *f = container_of(dcl, QemuFrames, dcl);

    frames_add_dirty(f, x, y, w, h);
}

static void frames_gfx_switch(DisplayChangeListener *dcl,
                              DisplaySurface *surface)
{
    QemuFrames *f = container_of(dcl, QemuFrames, dcl);
    int w, h;

    if (!surface) {
        return;
    }
    w = surface_width(surface);
    h = surface_height(surface);
    if (!f->prev || pixman_image_get_width(f->prev) != w ||
        pixman_image_get_height(f->prev) != h) {
        /* a new mode: every pixel counts as changed */
        if (f->prev) {
            QemuFramesUndo *u = g_new0(QemuFramesUndo, 1);

            u->box = (QemuFramesRect) { 0, 0,
                                        pixman_image_get_width(f->prev),
                                        pixman_image_get_height(f->prev) };
            u->pixels = u->box.w * u->box.h;
            u->whole = true;
            u->old = f->prev;
            frames_history_push(f, u);
            f->frame++;
            f->frame_ms = frames_now_ms();
        }
        f->prev = pixman_image_create_bits(PIXMAN_x8r8g8b8, w, h, NULL, 0);
    }
    frames_add_dirty(f, 0, 0, w, h);
}

static void frames_mouse_set(DisplayChangeListener *dcl, int x, int y,
                             bool on)
{
    QemuFrames *f = container_of(dcl, QemuFrames, dcl);

    f->cursor_valid = true;
    f->cursor_x = x;
    f->cursor_y = y;
    f->cursor_on = on;
}

static void frames_text_resize(DisplayChangeListener *dcl, int w, int h)
{
    QemuFrames *f = container_of(dcl, QemuFrames, dcl);

    f->text_w = w;
    f->text_h = h;
}

static void frames_text_cursor(DisplayChangeListener *dcl, int x, int y)
{
    QemuFrames *f = container_of(dcl, QemuFrames, dcl);

    f->text_cx = x;
    f->text_cy = y;
}

void qemu_frames_get_text(QemuFrames *f, int *w, int *h, int *cx, int *cy)
{
    *w = f->text_w;
    *h = f->text_h;
    *cx = f->text_cx;
    *cy = f->text_cy;
}

static const DisplayChangeListenerOps frames_ops = {
    .dpy_name       = "frames",
    .dpy_text_resize = frames_text_resize,
    .dpy_text_cursor = frames_text_cursor,
    .dpy_refresh    = frames_refresh,
    .dpy_gfx_update = frames_gfx_update,
    .dpy_gfx_switch = frames_gfx_switch,
    .dpy_mouse_set  = frames_mouse_set,
};

typedef struct QemuFramesEntry {
    QemuConsole *con;
    QemuFrames *frames;
    QLIST_ENTRY(QemuFramesEntry) node;
} QemuFramesEntry;

static QLIST_HEAD(, QemuFramesEntry) frames_list =
    QLIST_HEAD_INITIALIZER(frames_list);

QemuFrames *qemu_console_frames(QemuConsole *con)
{
    QemuFramesEntry *e;
    QemuFrames *f;

    QLIST_FOREACH(e, &frames_list, node) {
        if (e->con == con) {
            return e->frames;
        }
    }

    f = g_new0(QemuFrames, 1);
    f->text_cx = f->text_cy = -1;
    f->con = con;
    f->frame_ms = frames_now_ms();
    QTAILQ_INIT(&f->history);
    QLIST_INIT(&f->waiters);
    f->fast_until_ms = frames_now_ms() + FRAMES_ACTIVITY_MS;
    e = g_new0(QemuFramesEntry, 1);
    e->con = con;
    e->frames = f;
    QLIST_INSERT_HEAD(&frames_list, e, node);

    qemu_console_register_listener(con, &f->dcl, &frames_ops);
    /*
     * Catch up now: until there was a listener, the card may not have
     * drawn anything.  Nobody waits yet, so this is not a "change".
     */
    frames_refresh(&f->dcl);
    return f;
}

uint64_t qemu_frames_get_frame(QemuFrames *f)
{
    return f->frame;
}

void qemu_frames_activity(QemuFrames *f)
{
    f->fast_until_ms = frames_now_ms() + FRAMES_ACTIVITY_MS;
    frames_set_interval(f);
}

bool qemu_frames_get_hw_cursor(QemuFrames *f, int *x, int *y, bool *on)
{
    if (!f->cursor_valid) {
        return false;
    }
    *x = f->cursor_x;
    *y = f->cursor_y;
    *on = f->cursor_on;
    return true;
}

bool qemu_frames_get_pixel(QemuFrames *f, int x, int y, uint32_t *rgb)
{
    if (!f->prev || x < 0 || y < 0 ||
        x >= pixman_image_get_width(f->prev) ||
        y >= pixman_image_get_height(f->prev)) {
        return false;
    }
    *rgb = pixman_image_get_data(f->prev)[
        y * (pixman_image_get_stride(f->prev) / 4) + x] & 0xffffff;
    return true;
}

void qemu_frames_get_size(QemuFrames *f, int *w, int *h)
{
    *w = f->prev ? pixman_image_get_width(f->prev) : 0;
    *h = f->prev ? pixman_image_get_height(f->prev) : 0;
}

static bool frames_pixel_matches(QemuFrames *f, const QemuFramesCond *c)
{
    uint32_t rgb;
    int shift;

    if (!qemu_frames_get_pixel(f, c->pixel_x, c->pixel_y, &rgb)) {
        return false;
    }
    for (shift = 0; shift < 24; shift += 8) {
        int a = (rgb >> shift) & 0xff;
        int b = (c->pixel_rgb >> shift) & 0xff;

        if (abs(a - b) > c->pixel_tolerance) {
            return false;
        }
    }
    return true;
}

QemuFramesWaiter *qemu_frames_wait_begin(QemuFrames *f,
                                         const QemuFramesCond *c)
{
    QemuFramesWaiter *wt = g_new0(QemuFramesWaiter, 1);
    int64_t max_area = frames_anim_max_area(f);
    QemuFramesUndo *u;

    wt->cond = c;
    wt->begin_ms = frames_now_ms();
    wt->first_ms = -1;
    wt->last_ms = -1;
    /* an animation that runs already is known from the start */
    QTAILQ_FOREACH(u, &f->history, node) {
        QemuFramesRect box = u->box;
        bool ignored = false;
        int i;

        if (u->blink || u->whole || u->pixels < MAX(1, c->min_pixels) ||
            wt->begin_ms - u->ended_ms > FRAMES_ANIM_LOOKBACK_MS) {
            continue;
        }
        if (c->has_region && !frames_rect_clip(&box, &c->region)) {
            continue;
        }
        for (i = 0; i < c->n_ignore && !ignored; i++) {
            ignored = frames_rect_inside(&box, &c->ignore[i]);
        }
        if (!ignored) {
            frames_anim_feed(wt, &box, u->ended_ms, max_area);
        }
    }
    QLIST_INSERT_HEAD(&f->waiters, wt, node);
    frames_set_interval(f);
    return wt;
}

void coroutine_fn qemu_frames_co_wait_finish(QemuFrames *f,
                                             QemuFramesWaiter *wt,
                                             QemuFramesResult *r)
{
    const QemuFramesCond *c = wt->cond;
    int64_t start = frames_now_ms();
    int64_t now, deadline;
    int i;

    memset(r, 0, sizeof(*r));
    for (;;) {
        now = frames_now_ms();
        deadline = start + c->timeout_ms;

        if (c->cancelled && *c->cancelled) {
            r->cancelled = true;
            break;
        }
        switch (c->until) {
        case QEMU_FRAMES_CHANGE:
            r->matched = wt->first_ms >= 0;
            break;
        case QEMU_FRAMES_STABLE: {
            int64_t quiet = MAX(wt->last_ms, start);

            r->matched = now - quiet >= c->stable_ms;
            deadline = MIN(deadline, quiet + c->stable_ms);
            break;
        }
        case QEMU_FRAMES_CHANGE_THEN_STABLE:
            if (wt->first_ms >= 0) {
                r->matched = now - wt->last_ms >= c->stable_ms;
                deadline = MIN(deadline, wt->last_ms + c->stable_ms);
            }
            break;
        case QEMU_FRAMES_PIXEL:
            r->matched = frames_pixel_matches(f, c);
            break;
        }
        if (r->matched || now - start >= c->timeout_ms) {
            break;
        }
        qemu_co_sleep_ns_wakeable(c->sleep, QEMU_CLOCK_REALTIME,
                                  MAX(deadline - now, 1) * SCALE_MS);
    }

    QLIST_REMOVE(wt, node);
    qemu_frames_activity(f);

    r->elapsed_ms = now - wt->begin_ms;
    r->changed_after_ms = wt->first_ms >= 0 ? wt->first_ms - wt->begin_ms
                                            : -1;
    r->stable_after_ms = wt->last_ms >= 0 ? wt->last_ms - wt->begin_ms : -1;
    r->changed_pixels = wt->pixels;
    r->has_box = wt->has_box;
    r->box = wt->box;
    r->frame = f->frame;
    for (i = 0; i < QEMU_FRAMES_ANIM_MAX; i++) {
        if (frames_anim_seen(&wt->anim[i]) &&
            wt->anim[i].last_ms >= wt->begin_ms) {
            r->animations[r->n_animations++] = wt->anim[i].box;
        }
    }
    g_free(wt);
}

void qemu_frames_wait_abort(QemuFrames *f, QemuFramesWaiter *wt)
{
    QLIST_REMOVE(wt, node);
    g_free(wt);
    frames_set_interval(f);
}

void coroutine_fn qemu_frames_co_wait(QemuFrames *f, const QemuFramesCond *c,
                                      QemuFramesResult *r)
{
    qemu_frames_co_wait_finish(f, qemu_frames_wait_begin(f, c), r);
}

int qemu_frames_history(QemuFrames *f, QemuFramesInfo *out, int max)
{
    QemuFramesUndo *u;
    int64_t now = frames_now_ms();
    int n = 0;

    if (max > 0 && f->prev) {
        out[n++] = (QemuFramesInfo) {
            .frame = f->frame,
            .ago_ms = now - f->frame_ms,
            .shown_ms = now - f->frame_ms,
            .current = true,
            .w = pixman_image_get_width(f->prev),
            .h = pixman_image_get_height(f->prev),
        };
    }
    QTAILQ_FOREACH_REVERSE(u, &f->history, node) {
        if (n >= max) {
            break;
        }
        if (u->blink) {
            continue;
        }
        out[n++] = (QemuFramesInfo) {
            .frame = u->frame,
            .ago_ms = now - u->shown_ms,
            .shown_ms = u->ended_ms - u->shown_ms,
            .box = u->box,
            .pixels = u->pixels,
            .w = u->whole ? u->box.w : 0,
            .h = u->whole ? u->box.h : 0,
        };
    }
    return n;
}

pixman_image_t *qemu_frames_get_image(QemuFrames *f, uint64_t frame)
{
    QemuFramesUndo *u;
    pixman_image_t *img;

    if (!f->prev || frame > f->frame) {
        return NULL;
    }
    u = QTAILQ_FIRST(&f->history);
    if (frame != f->frame && (!u || frame < u->frame)) {
        return NULL;            /* too old, dropped */
    }
    img = pixman_image_create_bits(PIXMAN_x8r8g8b8,
                                   pixman_image_get_width(f->prev),
                                   pixman_image_get_height(f->prev), NULL, 0);
    pixman_image_composite(PIXMAN_OP_SRC, f->prev, NULL, img, 0, 0, 0, 0,
                           0, 0, pixman_image_get_width(f->prev),
                           pixman_image_get_height(f->prev));
    QTAILQ_FOREACH_REVERSE(u, &f->history, node) {
        if (u->frame < frame) {
            break;
        }
        if (u->whole) {
            qemu_pixman_image_unref(img);
            img = pixman_image_create_bits(PIXMAN_x8r8g8b8, u->box.w,
                                           u->box.h, NULL, 0);
            pixman_image_composite(PIXMAN_OP_SRC, u->old, NULL, img,
                                   0, 0, 0, 0, 0, 0, u->box.w, u->box.h);
        } else {
            pixman_image_composite(PIXMAN_OP_SRC, u->old, NULL, img,
                                   0, 0, 0, 0, u->box.x, u->box.y,
                                   u->box.w, u->box.h);
        }
    }
    return img;
}

static Notifier frames_init_done;

static void frames_track_all(Notifier *n, void *data)
{
    QemuConsole *con;
    int i;

    for (i = 0; (con = qemu_console_lookup_by_index(i)); i++) {
        if (qemu_console_is_graphic(con)) {
            qemu_console_frames(con);
        }
    }
}

void qemu_frames_track_all(void)
{
    frames_init_done.notify = frames_track_all;
    qemu_add_machine_init_done_notifier(&frames_init_done);
}
