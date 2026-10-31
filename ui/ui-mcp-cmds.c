/*
 * Screen and input commands for MCP clients (see qapi/mcp.json)
 *
 * They are ordinary QMP commands too.  Called through the MCP monitor,
 * their input belongs to the MCP call, so a cancel or a disconnect
 * stops it and releases every key and button it pressed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "keymaps.h"
#include "monitor/mcp.h"
#include "qapi/clone-visitor.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-mcp.h"
#include "qapi/qapi-visit-ui.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "qobject/qjson.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"
#include "system/runstate.h"
#include "ui/console.h"
#include "ui/frames.h"
#include "ui/input.h"
#include "ui/png.h"
#include "ui/pointer.h"
#include "vnc_keysym.h"

#define MCP_WAIT_MAX_MS     110000
#define MCP_MAX_KEYS        100000

/* Calls */

/*
 * The input context: the MCP call's, or @local for a QMP caller.
 * Declare @local with g_auto(QemuInputCtx) so it ends with the call.
 */
static QemuInputCtx *mcp_ctx(QemuInputCtx *local)
{
    QemuInputCtx *ctx = mcp_current_input_ctx();

    if (ctx) {
        return ctx;
    }
    qemu_input_ctx_init(local, NULL);
    return local;
}

static int64_t mcp_now_ms(void)
{
    return qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
}

/* Sleep @ms of real time; false if the call was cancelled */
static bool coroutine_fn mcp_co_sleep(QemuInputCtx *ctx, int64_t ms)
{
    int64_t end = mcp_now_ms() + ms;

    while (!ctx->cancelled && mcp_now_ms() < end) {
        qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME,
                                  (end - mcp_now_ms()) * SCALE_MS);
    }
    return !ctx->cancelled;
}

/* Consoles */

static char *mcp_console_list(void)
{
    GString *s = g_string_new(NULL);
    QemuConsole *con;
    int i;

    for (i = 0; (con = qemu_console_lookup_by_index(i)); i++) {
        g_autofree char *label = qemu_console_get_label(con);

        g_string_append_printf(s, "%s%d = %s %dx%d", i ? ", " : "", i, label,
                               qemu_console_get_width(con, 0),
                               qemu_console_get_height(con, 0));
    }
    return g_string_free(s, false);
}

static QemuConsole *mcp_console(QObject *arg, int head, Error **errp)
{
    QemuConsole *con = NULL;
    QNum *num = qobject_to(QNum, arg);
    QString *str = qobject_to(QString, arg);
    int64_t index;

    if (!arg) {
        con = qemu_console_lookup_default();
    } else if (num && qnum_get_try_int(num, &index)) {
        con = index >= 0 ? qemu_console_lookup_by_index(index) : NULL;
    } else if (str) {
        const char *s = qstring_get_str(str);

        if (!qemu_strtoi64(s, NULL, 10, &index)) {
            con = index >= 0 ? qemu_console_lookup_by_index(index) : NULL;
        } else {
            con = qemu_console_lookup_by_device_name(s, head, NULL);
        }
    } else {
        error_setg(errp, "console must be an index (number) or a device "
                   "id (string)");
        return NULL;
    }
    if (!con) {
        g_autofree char *list = mcp_console_list();
        g_autofree char *which = NULL;

        if (arg) {
            GString *json = qobject_to_json(arg);

            which = g_string_free(json, false);
        }
        error_setg(errp, "console %s not found; consoles: %s",
                   which ? which : "(default)", *list ? list : "none");
    }
    return con;
}

/* Screenshots */

static void mcp_draw_pointer(pixman_image_t *img, QemuConsole *con,
                             ScreenRegion *region, double scale)
{
    pixman_color_t red = { 0xffff, 0, 0, 0xffff };
    pixman_color_t white = { 0xffff, 0xffff, 0xffff, 0xffff };
    QemuPointerMethod method;
    pixman_rectangle16_t r[2];
    int x, y;

    if (!qemu_pointer_get(con, &x, &y, &method)) {
        return;
    }
    if (region) {
        x -= region->x;
        y -= region->y;
    }
    x = x * scale;
    y = y * scale;
    /* a white cross with a red centre line */
    r[0] = (pixman_rectangle16_t) { x - 7, y - 1, 15, 3 };
    r[1] = (pixman_rectangle16_t) { x - 1, y - 7, 3, 15 };
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, img, &white, 2, r);
    r[0] = (pixman_rectangle16_t) { x - 6, y, 13, 1 };
    r[1] = (pixman_rectangle16_t) { x, y - 6, 1, 13 };
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, img, &red, 2, r);
}

/* PNG of @img, a capture of a @screen_w x @screen_h screen */
static McpImage *mcp_image_encode(pixman_image_t *img, int screen_w,
                                  int screen_h, ScreenRegion *region,
                                  double scale, Error **errp)
{
    g_autoptr(GByteArray) png = qemu_png_encode(img, errp);
    McpImage *mi;

    if (!png) {
        return NULL;
    }
    mi = g_new0(McpImage, 1);
    mi->data = g_base64_encode(png->data, png->len);
    mi->mime_type = g_strdup("image/png");
    mi->width = pixman_image_get_width(img);
    mi->height = pixman_image_get_height(img);
    mi->screen_width = screen_w;
    mi->screen_height = screen_h;
    mi->region = region ? QAPI_CLONE(ScreenRegion, region) : NULL;
    mi->scale = scale;
    return mi;
}

static McpImage *coroutine_fn mcp_co_image(QemuConsole *con,
                                           ScreenRegion *region,
                                           double scale, bool pointer,
                                           Error **errp)
{
    g_autoptr(pixman_image_t) img = NULL;

    img = qemu_console_co_capture(con, region, scale, errp);
    if (!img) {
        return NULL;
    }
    if (pointer) {
        mcp_draw_pointer(img, con, region, scale);
    }
    return mcp_image_encode(img, qemu_console_get_width(con, 0),
                            qemu_console_get_height(con, 0), region, scale,
                            errp);
}

/* A frame from the history, cropped and scaled like a capture */
static McpImage *mcp_frame_image(QemuConsole *con, uint64_t frame,
                                 ScreenRegion *region, double scale,
                                 Error **errp)
{
    g_autoptr(pixman_image_t) full = NULL;
    g_autoptr(pixman_image_t) img = NULL;
    int w, h, x = 0, y = 0, rw, rh;

    if (scale < 0.1 || scale > 2.0) {
        error_setg(errp, "scale %g is out of range; use 0.1 to 2.0", scale);
        return NULL;
    }
    full = qemu_frames_get_image(qemu_console_frames(con), frame);
    if (!full) {
        error_setg(errp, "frame %" PRIu64 " is not in the history; use the "
                   "frames tool to list the frames kept", frame);
        return NULL;
    }
    w = rw = pixman_image_get_width(full);
    h = rh = pixman_image_get_height(full);
    if (region) {
        if (region->x < 0 || region->y < 0 || region->w <= 0 ||
            region->h <= 0 || region->x + region->w > w ||
            region->y + region->h > h) {
            error_setg(errp, "region %" PRId64 ",%" PRId64 " %" PRId64
                       "x%" PRId64 " is outside the %dx%d frame",
                       region->x, region->y, region->w, region->h, w, h);
            return NULL;
        }
        x = region->x;
        y = region->y;
        rw = region->w;
        rh = region->h;
    }
    img = qemu_pixman_crop_scale(full, x, y, rw, rh, scale);
    return mcp_image_encode(img, w, h, region, scale, errp);
}

/* Does @choice ask for a screenshot?  Fills @region and @scale. */
static bool mcp_screenshot_wanted(McpScreenshotChoice *choice, bool dflt,
                                  ScreenRegion **region, double *scale)
{
    *region = NULL;
    *scale = 1.0;
    if (!choice) {
        return dflt;
    }
    if (choice->type == QTYPE_QBOOL) {
        return choice->u.enabled;
    }
    *region = choice->u.options.region;
    if (choice->u.options.has_scale) {
        *scale = choice->u.options.scale;
    }
    return true;
}

McpResult *coroutine_fn qmp_screenshot(QObject *console, bool has_head,
                                       int64_t head, ScreenRegion *region,
                                       bool has_scale, double scale,
                                       bool has_pointer, bool pointer,
                                       bool has_frame, int64_t frame,
                                       Error **errp)
{
    QemuConsole *con = mcp_console(console, has_head ? head : 0, errp);
    McpResult *res;
    McpImage *mi;

    if (!con) {
        return NULL;
    }
    if (has_frame) {
        mi = mcp_frame_image(con, frame, region, has_scale ? scale : 1.0,
                             errp);
    } else {
        mi = mcp_co_image(con, region, has_scale ? scale : 1.0,
                          has_pointer && pointer, errp);
    }
    if (!mi) {
        return NULL;
    }
    res = g_new0(McpResult, 1);
    res->summary = g_strdup_printf("console %d, screen %" PRId64 "x%" PRId64
                                   ", image %" PRId64 "x%" PRId64,
                                   qemu_console_get_index(con),
                                   mi->screen_width, mi->screen_height,
                                   mi->width, mi->height);
    QAPI_LIST_PREPEND(res->images, mi);
    res->has_images = true;
    return res;
}

/* Waits */

static McpWaitInfo *mcp_wait_info(const QemuFramesResult *r)
{
    McpWaitInfo *w = g_new0(McpWaitInfo, 1);
    int i;

    w->matched = r->matched;
    w->elapsed_ms = r->elapsed_ms;
    if (r->changed_after_ms >= 0) {
        w->has_changed_after_ms = true;
        w->changed_after_ms = r->changed_after_ms;
        w->has_stable_after_ms = true;
        w->stable_after_ms = r->stable_after_ms;
    }
    w->changed_pixels = r->changed_pixels;
    if (r->has_box) {
        w->box = g_new0(ScreenRegion, 1);
        w->box->x = r->box.x;
        w->box->y = r->box.y;
        w->box->w = r->box.w;
        w->box->h = r->box.h;
    }
    w->frame = r->frame;
    for (i = r->n_animations - 1; i >= 0; i--) {
        ScreenRegion *a = g_new0(ScreenRegion, 1);

        a->x = r->animations[i].x;
        a->y = r->animations[i].y;
        a->w = r->animations[i].w;
        a->h = r->animations[i].h;
        QAPI_LIST_PREPEND(w->animations, a);
        w->has_animations = true;
    }
    return w;
}

static void mcp_wait_summary(GString *s, QemuFramesUntil until,
                             bool ignore_animation,
                             const QemuFramesResult *r)
{
    if (r->n_animations && ignore_animation) {
        g_string_append_printf(s, "ignoring %d animated area%s; ",
                               r->n_animations,
                               r->n_animations > 1 ? "s" : "");
    }
    if (r->matched) {
        switch (until) {
        case QEMU_FRAMES_CHANGE:
            g_string_append_printf(s, "screen changed after %" PRId64 " ms",
                                   r->changed_after_ms);
            break;
        case QEMU_FRAMES_STABLE:
            g_string_append_printf(s, "screen stable after %" PRId64 " ms",
                                   r->elapsed_ms);
            break;
        case QEMU_FRAMES_CHANGE_THEN_STABLE:
            g_string_append_printf(s, "screen changed after %" PRId64
                                   " ms, settled at %" PRId64 " ms",
                                   r->changed_after_ms, r->stable_after_ms);
            break;
        case QEMU_FRAMES_PIXEL:
            g_string_append_printf(s, "pixel matched after %" PRId64 " ms",
                                   r->elapsed_ms);
            break;
        }
        return;
    }
    switch (until) {
    case QEMU_FRAMES_CHANGE:
        g_string_append_printf(s, "no change within %" PRId64 " ms",
                               r->elapsed_ms);
        break;
    case QEMU_FRAMES_STABLE:
        g_string_append_printf(s, "screen still changing after %" PRId64
                               " ms", r->elapsed_ms);
        break;
    case QEMU_FRAMES_CHANGE_THEN_STABLE:
        if (r->changed_after_ms < 0) {
            g_string_append_printf(s, "no change within %" PRId64 " ms",
                                   r->elapsed_ms);
        } else {
            g_string_append_printf(s, "screen changed after %" PRId64
                                   " ms but was still changing after %"
                                   PRId64 " ms", r->changed_after_ms,
                                   r->elapsed_ms);
        }
        break;
    case QEMU_FRAMES_PIXEL:
        g_string_append_printf(s, "pixel did not match within %" PRId64
                               " ms", r->elapsed_ms);
        return;
    }
    if (r->n_animations && !ignore_animation) {
        const QemuFramesRect *a = &r->animations[0];

        g_string_append_printf(s, "; %d,%d %dx%d keeps changing on its own "
                               "(a clock or a graph?): ignore-animation "
                               "would skip it", a->x, a->y, a->w, a->h);
    }
}

static bool mcp_until(McpWaitUntil until, QemuFramesUntil *out,
                      Error **errp)
{
    switch (until) {
    case MCP_WAIT_UNTIL_CHANGE:
        *out = QEMU_FRAMES_CHANGE;
        return true;
    case MCP_WAIT_UNTIL_STABLE:
        *out = QEMU_FRAMES_STABLE;
        return true;
    case MCP_WAIT_UNTIL_CHANGE_THEN_STABLE:
        *out = QEMU_FRAMES_CHANGE_THEN_STABLE;
        return true;
    case MCP_WAIT_UNTIL_PIXEL:
        *out = QEMU_FRAMES_PIXEL;
        return true;
    default:
        error_setg(errp, "nothing to wait for");
        return false;
    }
}

static bool mcp_check_rect(const ScreenRegion *r, QemuConsole *con,
                           const char *what, Error **errp)
{
    int w = qemu_console_get_width(con, 0);
    int h = qemu_console_get_height(con, 0);

    if (r->x < 0 || r->y < 0 || r->w <= 0 || r->h <= 0 ||
        r->x + r->w > w || r->y + r->h > h) {
        error_setg(errp, "%s %" PRId64 ",%" PRId64 " %" PRId64 "x%" PRId64
                   " is outside the %dx%d screen", what, r->x, r->y, r->w,
                   r->h, w, h);
        return false;
    }
    return true;
}

McpWaitResult *coroutine_fn qmp_wait_screen(McpWaitUntil until,
                                            QObject *console,
                                            ScreenRegion *region,
                                            bool has_ignore,
                                            ScreenRegionList *ignore,
                                            bool has_min_pixels,
                                            int64_t min_pixels,
                                            bool has_stable_ms,
                                            int64_t stable_ms,
                                            bool has_timeout_ms,
                                            int64_t timeout_ms,
                                            McpPixel *pixel,
                                            bool has_ignore_blink,
                                            bool ignore_blink,
                                            bool has_ignore_animation,
                                            bool ignore_animation,
                                            McpScreenshotChoice *screenshot,
                                            Error **errp)
{
    QemuConsole *con = mcp_console(console, 0, errp);
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_ctx(&local);
    g_autofree QemuFramesRect *ign = NULL;
    g_autoptr(GString) summary = g_string_new(NULL);
    QemuFramesCond c = { 0 };
    QemuFramesResult r;
    ScreenRegionList *l;
    McpWaitResult *res;
    ScreenRegion *shot_region;
    double shot_scale;

    if (!con || !mcp_until(until, &c.until, errp)) {
        return NULL;
    }
    if (region) {
        if (!mcp_check_rect(region, con, "region", errp)) {
            return NULL;
        }
        c.has_region = true;
        c.region = (QemuFramesRect) { region->x, region->y,
                                      region->w, region->h };
    }
    for (l = ignore; l; l = l->next) {
        ign = g_renew(QemuFramesRect, ign, c.n_ignore + 1);
        ign[c.n_ignore++] = (QemuFramesRect) { l->value->x, l->value->y,
                                               l->value->w, l->value->h };
    }
    c.ignore = ign;
    c.min_pixels = has_min_pixels ? min_pixels : 64;
    c.stable_ms = has_stable_ms ? stable_ms : 2000;
    c.timeout_ms = MIN(has_timeout_ms ? timeout_ms : 60000, MCP_WAIT_MAX_MS);
    c.ignore_blink = has_ignore_blink ? ignore_blink : true;
    c.ignore_animation = has_ignore_animation && ignore_animation;
    c.sleep = &ctx->sleep;
    c.cancelled = &ctx->cancelled;
    if (c.until == QEMU_FRAMES_PIXEL) {
        unsigned int rgb;

        if (!pixel) {
            error_setg(errp, "'pixel' is needed for until=pixel");
            return NULL;
        }
        if (sscanf(pixel->rgb, "#%6x", &rgb) != 1 ||
            strlen(pixel->rgb) != 7) {
            error_setg(errp, "rgb must look like \"#rrggbb\"");
            return NULL;
        }
        c.pixel_x = pixel->x;
        c.pixel_y = pixel->y;
        c.pixel_rgb = rgb;
        c.pixel_tolerance = pixel->has_tolerance ? pixel->tolerance : 16;
    }

    qemu_frames_co_wait(qemu_console_frames(con), &c, &r);
    if (r.cancelled) {
        error_setg(errp, "cancelled");
        return NULL;
    }

    res = g_new0(McpWaitResult, 1);
    res->wait = mcp_wait_info(&r);
    mcp_wait_summary(summary, c.until, c.ignore_animation, &r);
    if (mcp_screenshot_wanted(screenshot, true, &shot_region, &shot_scale)) {
        McpImage *mi = mcp_co_image(con, shot_region, shot_scale, false,
                                    errp);

        if (!mi) {
            qapi_free_McpWaitResult(res);
            return NULL;
        }
        QAPI_LIST_PREPEND(res->images, mi);
        res->has_images = true;
    }
    res->summary = g_strdup(summary->str);
    return res;
}

/* Input: what every input tool does around its input */

typedef struct McpInput {
    QemuConsole *con;
    QemuInputCtx *ctx;
    McpThen *then;
    QemuFrames *frames;
    QemuFramesWaiter *wt;
    QemuFramesCond cond;
    int64_t start_ms;
} McpInput;

static bool mcp_input_begin(McpInput *in, QemuConsole *con, McpThen *then,
                            QemuInputCtx *ctx, Error **errp)
{
    memset(in, 0, sizeof(*in));
    in->con = con;
    in->ctx = ctx;
    in->then = then;
    in->start_ms = mcp_now_ms();

    if (!runstate_is_running()) {
        error_setg(errp, "VM is paused (state %s); resume it first",
                   RunState_str(runstate_get()));
        return false;
    }
    /* start tracking frames before the input, so no change is missed */
    in->frames = qemu_console_frames(con);
    qemu_frames_activity(in->frames);

    if (then && then->has_wait && then->wait != MCP_WAIT_UNTIL_NONE) {
        if (then->wait == MCP_WAIT_UNTIL_PIXEL) {
            error_setg(errp, "then.wait cannot be \"pixel\"; use "
                       "wait-screen");
            return false;
        }
        mcp_until(then->wait, &in->cond.until, &error_abort);
        in->cond.min_pixels = 64;
        in->cond.stable_ms = then->has_stable_ms ? then->stable_ms : 1000;
        in->cond.timeout_ms = MIN(then->has_timeout_ms ? then->timeout_ms
                                                       : 10000,
                                  MCP_WAIT_MAX_MS);
        in->cond.ignore_blink = true;
        in->cond.ignore_animation = then->has_ignore_animation &&
                                    then->ignore_animation;
        in->cond.sleep = &ctx->sleep;
        in->cond.cancelled = &ctx->cancelled;
        in->wt = qemu_frames_wait_begin(in->frames, &in->cond);
    }
    return true;
}

static void mcp_input_abort(McpInput *in)
{
    if (in->wt) {
        qemu_frames_wait_abort(in->frames, in->wt);
        in->wt = NULL;
    }
}

/* After the input: delay, wait, screenshot; fill in the result */
static bool coroutine_fn mcp_co_input_finish(McpInput *in,
                                             McpInputResult *res,
                                             GString *summary, Error **errp)
{
    McpThen *then = in->then;
    ScreenRegion *region;
    double scale;

    if (then && then->has_delay_ms && then->delay_ms > 0 &&
        !mcp_co_sleep(in->ctx, then->delay_ms)) {
        mcp_input_abort(in);
        error_setg(errp, "cancelled");
        return false;
    }
    if (in->wt) {
        QemuFramesResult r;

        qemu_frames_co_wait_finish(in->frames, in->wt, &r);
        in->wt = NULL;
        if (r.cancelled) {
            error_setg(errp, "cancelled");
            return false;
        }
        res->wait = mcp_wait_info(&r);
        g_string_append(summary, "; ");
        mcp_wait_summary(summary, in->cond.until, in->cond.ignore_animation,
                         &r);
    }
    if (then && mcp_screenshot_wanted(then->screenshot, false,
                                      &region, &scale)) {
        McpImage *mi = mcp_co_image(in->con, region, scale, false, errp);

        if (!mi) {
            return false;
        }
        QAPI_LIST_PREPEND(res->images, mi);
        res->has_images = true;
    }
    res->elapsed_ms = mcp_now_ms() - in->start_ms;
    res->summary = g_strdup(summary->str);
    return true;
}

/* Keys */

static const struct {
    const char *alias;
    const char *name;
} mcp_key_aliases[] = {
    { "enter", "ret" },
    { "return", "ret" },
    { "space", "spc" },
    { "escape", "esc" },
    { "del", "delete" },
    { "bksp", "backspace" },
    { "pageup", "pgup" },
    { "pagedown", "pgdn" },
    { "win", "meta_l" },
    { "super", "meta_l" },
    { "control", "ctrl" },
    { "ins", "insert" },
    { "altgr", "alt_r" },
    { "capslock", "caps_lock" },
    { "printscreen", "print" },
};

static int mcp_key_code(const char *name)
{
    g_autofree char *lower = g_ascii_strdown(name, -1);
    int i;

    for (i = 0; i < ARRAY_SIZE(mcp_key_aliases); i++) {
        if (!strcmp(lower, mcp_key_aliases[i].alias)) {
            return qapi_enum_parse(&QKeyCode_lookup, mcp_key_aliases[i].name,
                                   -1, NULL);
        }
    }
    return qapi_enum_parse(&QKeyCode_lookup, lower, -1, NULL);
}

/* Parse "ctrl-alt-delete" or "down*5" into Linux key codes */
static int mcp_parse_combo(const char *spec, unsigned int *codes, int max,
                           int *repeat, Error **errp)
{
    g_autofree char *s = g_strdup(spec);
    char *star = strrchr(s, '*');
    char **parts;
    int n = 0, i;

    *repeat = 1;
    if (star && star != s && star[1]) {
        if (qemu_strtoi(star + 1, NULL, 10, repeat) || *repeat < 1 ||
            *repeat > 1000) {
            error_setg(errp, "bad repeat count in '%s'", spec);
            return -1;
        }
        *star = '\0';
    }
    parts = g_strsplit_set(s, "-+", -1);
    for (i = 0; parts[i]; i++) {
        int q;

        if (!*parts[i]) {
            continue;
        }
        q = mcp_key_code(parts[i]);
        if (q < 0) {
            error_setg(errp, "unknown key '%s' in '%s'; use QEMU key names "
                       "such as ret, spc, esc, tab, ctrl, alt, shift, a, 1",
                       parts[i], spec);
            g_strfreev(parts);
            return -1;
        }
        if (n == max) {
            error_setg(errp, "too many keys in '%s'", spec);
            g_strfreev(parts);
            return -1;
        }
        codes[n++] = qemu_input_map_qcode_to_linux[q];
    }
    g_strfreev(parts);
    if (!n) {
        error_setg(errp, "empty key '%s'", spec);
    }
    return n ? n : -1;
}

static void mcp_add_combo(QemuInputBatch *b, const unsigned int *codes, int n,
                          int lead_ms, int hold_ms, int delay_ms)
{
    int i;

    for (i = 0; i < n - 1; i++) {
        qemu_input_batch_key(b, codes[i], true);
        qemu_input_batch_delay(b, lead_ms);
    }
    qemu_input_batch_key(b, codes[n - 1], true);
    qemu_input_batch_delay(b, hold_ms);
    qemu_input_batch_key(b, codes[n - 1], false);
    for (i = n - 2; i >= 0; i--) {
        qemu_input_batch_key(b, codes[i], false);
    }
    qemu_input_batch_delay(b, delay_ms);
}

static void mcp_caps_warning(McpInputResult *res, QemuConsole *con)
{
    if (qemu_input_get_leds_mask(con) & QEMU_CAPS_LOCK_LED) {
        res->warning = g_strdup("Caps Lock is on in the guest");
    }
}

McpInputResult *coroutine_fn qmp_key(strList *keys, bool has_delay_ms,
                                     int64_t delay_ms, bool has_hold_ms,
                                     int64_t hold_ms,
                                     bool has_modifier_lead_ms,
                                     int64_t modifier_lead_ms,
                                     QObject *console, McpThen *then,
                                     Error **errp)
{
    QemuConsole *con = mcp_console(console, 0, errp);
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_ctx(&local);
    g_autoptr(GString) summary = g_string_new(NULL);
    QemuInputBatch *b;
    McpInputResult *res;
    McpInput in;
    strList *k;
    int combos = 0;

    if (!con) {
        return NULL;
    }
    if (!keys) {
        error_setg(errp, "'keys' is empty");
        return NULL;
    }
    /* check everything before pressing anything */
    b = qemu_input_batch_new(con);
    for (k = keys; k; k = k->next) {
        unsigned int codes[8];
        int repeat, n, i;

        n = mcp_parse_combo(k->value, codes, ARRAY_SIZE(codes), &repeat,
                            errp);
        if (n < 0) {
            qemu_input_batch_free(b);
            return NULL;
        }
        for (i = 0; i < repeat; i++) {
            mcp_add_combo(b, codes, n,
                          has_modifier_lead_ms ? modifier_lead_ms : 20,
                          has_hold_ms ? hold_ms : 40,
                          has_delay_ms ? delay_ms : 80);
            combos++;
        }
    }
    if (!mcp_input_begin(&in, con, then, ctx, errp)) {
        qemu_input_batch_free(b);
        return NULL;
    }
    if (!qemu_input_co_run(ctx, b, errp)) {
        mcp_input_abort(&in);
        return NULL;
    }
    res = g_new0(McpInputResult, 1);
    g_string_printf(summary, "sent %d key combination%s", combos,
                    combos == 1 ? "" : "s");
    mcp_caps_warning(res, con);
    if (!mcp_co_input_finish(&in, res, summary, errp)) {
        qapi_free_McpInputResult(res);
        return NULL;
    }
    return res;
}

/* Text */

typedef struct McpChar {
    unsigned int lnx;
    bool shift;
    bool altgr;
} McpChar;

static const char mcp_us_shifted_digits[] = ")!@#$%^&*(";
static const struct {
    char plain, shifted;
    const char *name;
} mcp_us_keys[] = {
    { '`', '~', "grave_accent" },
    { '-', '_', "minus" },
    { '=', '+', "equal" },
    { '[', '{', "bracket_left" },
    { ']', '}', "bracket_right" },
    { '\\', '|', "backslash" },
    { ';', ':', "semicolon" },
    { '\'', '"', "apostrophe" },
    { ',', '<', "comma" },
    { '.', '>', "dot" },
    { '/', '?', "slash" },
};

static bool mcp_char_us(gunichar c, McpChar *out)
{
    const char *name = NULL;
    char buf[2] = { 0 };
    /* the shifted digit row: ')' is on 0, '!' on 1 and so on */
    const char *p = c && c < 0x80 ? strchr(mcp_us_shifted_digits, c) : NULL;
    int i;

    memset(out, 0, sizeof(*out));
    if (c == '\n' || c == '\r') {
        name = "ret";
    } else if (c == '\t') {
        name = "tab";
    } else if (c == ' ') {
        name = "spc";
    } else if (c >= 'a' && c <= 'z') {
        buf[0] = c;
        name = buf;
    } else if (c >= 'A' && c <= 'Z') {
        buf[0] = c - 'A' + 'a';
        name = buf;
        out->shift = true;
    } else if (c >= '0' && c <= '9') {
        buf[0] = c;
        name = buf;
    } else if (p) {
        buf[0] = '0' + (p - mcp_us_shifted_digits);
        name = buf;
        out->shift = true;
    } else {
        for (i = 0; i < ARRAY_SIZE(mcp_us_keys); i++) {
            if (c == mcp_us_keys[i].plain || c == mcp_us_keys[i].shifted) {
                name = mcp_us_keys[i].name;
                out->shift = c == mcp_us_keys[i].shifted;
                break;
            }
        }
    }
    if (!name) {
        return false;
    }
    out->lnx = qemu_input_map_qcode_to_linux[
        qapi_enum_parse(&QKeyCode_lookup, name, -1, &error_abort)];
    return true;
}

static kbd_layout_t *mcp_layout(const char *name, Error **errp)
{
    static GHashTable *layouts;
    kbd_layout_t *k;

    if (!layouts) {
        layouts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        NULL);
    }
    k = g_hash_table_lookup(layouts, name);
    if (!k) {
        if (strchr(name, '/') || strchr(name, '.')) {
            error_setg(errp, "bad keyboard layout name '%s'", name);
            return NULL;
        }
        k = kbd_layout_new(name2keysym, name, errp);
        if (k) {
            g_hash_table_insert(layouts, g_strdup(name), k);
        }
    }
    return k;
}

static bool mcp_char_layout(kbd_layout_t *k, gunichar c, McpChar *out)
{
    uint16_t codes[4];
    int keysym, n, i, best = -1, best_score = INT_MAX;

    memset(out, 0, sizeof(*out));
    if (c == '\n' || c == '\r') {
        keysym = 0xff0d;            /* Return */
    } else if (c == '\t') {
        keysym = 0xff09;            /* Tab */
    } else {
        keysym = c;
    }
    n = keysym2scancodes(k, keysym, codes, ARRAY_SIZE(codes));
    if (!n && c > 0xff) {
        n = keysym2scancodes(k, 0x1000000 | c, codes, ARRAY_SIZE(codes));
    }
    for (i = 0; i < n; i++) {
        int score = 0;

        if (codes[i] & SCANCODE_CTRL) {
            continue;
        }
        score += (codes[i] & SCANCODE_SHIFT) ? 1 : 0;
        score += (codes[i] & SCANCODE_ALTGR) ? 2 : 0;
        score += keycode_is_keypad(k, codes[i] & SCANCODE_KEYMASK) ? 10 : 0;
        if (score < best_score) {
            best = i;
            best_score = score;
        }
    }
    if (best < 0) {
        return false;
    }
    out->lnx = qemu_input_key_number_to_linux(codes[best] & SCANCODE_KEYMASK);
    out->shift = codes[best] & SCANCODE_SHIFT;
    out->altgr = codes[best] & SCANCODE_ALTGR;
    return out->lnx != 0;
}

McpInputResult *coroutine_fn qmp_type_text(const char *text,
                                           const char *layout,
                                           bool has_char_delay_ms,
                                           int64_t char_delay_ms,
                                           bool has_hold_ms, int64_t hold_ms,
                                           bool has_modifier_lead_ms,
                                           int64_t modifier_lead_ms,
                                           QObject *console, McpThen *then,
                                           Error **errp)
{
    QemuConsole *con = mcp_console(console, 0, errp);
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_ctx(&local);
    g_autoptr(GString) summary = g_string_new(NULL);
    unsigned int shift = qemu_input_map_qcode_to_linux[Q_KEY_CODE_SHIFT];
    unsigned int altgr = qemu_input_map_qcode_to_linux[Q_KEY_CODE_ALT_R];
    int lead = has_modifier_lead_ms ? modifier_lead_ms : 20;
    int hold = has_hold_ms ? hold_ms : 20;
    int delay = has_char_delay_ms ? char_delay_ms : 30;
    kbd_layout_t *k = NULL;
    QemuInputBatch *b;
    McpInputResult *res;
    McpInput in;
    const char *p;
    long count = 0;

    if (!con) {
        return NULL;
    }
    if (!g_utf8_validate(text, -1, NULL)) {
        error_setg(errp, "text is not valid UTF-8");
        return NULL;
    }
    if (g_utf8_strlen(text, -1) > MCP_MAX_KEYS) {
        error_setg(errp, "text is too long (at most %d characters)",
                   MCP_MAX_KEYS);
        return NULL;
    }
    if (layout && strcmp(layout, "en-us") && strcmp(layout, "us")) {
        k = mcp_layout(layout, errp);
        if (!k) {
            return NULL;
        }
    }

    /* check every character before typing any */
    b = qemu_input_batch_new(con);
    for (p = text; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        McpChar mc;
        bool ok = k ? mcp_char_layout(k, c, &mc) : mcp_char_us(c, &mc);

        if (!ok) {
            char buf[8] = { 0 };

            g_unichar_to_utf8(c, buf);
            error_setg(errp, "character %ld ('%s', U+%04X) cannot be typed "
                       "with layout %s; nothing was typed",
                       count + 1, c < 0x20 ? "?" : buf, c,
                       layout ? layout : "en-us");
            qemu_input_batch_free(b);
            return NULL;
        }
        if (mc.shift) {
            qemu_input_batch_key(b, shift, true);
            qemu_input_batch_delay(b, lead);
        }
        if (mc.altgr) {
            qemu_input_batch_key(b, altgr, true);
            qemu_input_batch_delay(b, lead);
        }
        qemu_input_batch_key(b, mc.lnx, true);
        qemu_input_batch_delay(b, hold);
        qemu_input_batch_key(b, mc.lnx, false);
        if (mc.altgr) {
            qemu_input_batch_key(b, altgr, false);
        }
        if (mc.shift) {
            qemu_input_batch_key(b, shift, false);
        }
        qemu_input_batch_delay(b, delay);
        count++;
    }

    if (!mcp_input_begin(&in, con, then, ctx, errp)) {
        qemu_input_batch_free(b);
        return NULL;
    }
    if (!qemu_input_co_run(ctx, b, errp)) {
        mcp_input_abort(&in);
        return NULL;
    }
    res = g_new0(McpInputResult, 1);
    g_string_printf(summary, "typed %ld character%s", count,
                    count == 1 ? "" : "s");
    mcp_caps_warning(res, con);
    if (!mcp_co_input_finish(&in, res, summary, errp)) {
        qapi_free_McpInputResult(res);
        return NULL;
    }
    return res;
}

/* Mouse */

static QemuPointerMethod mcp_method(bool has_method, McpPointerMethod m)
{
    switch (has_method ? m : MCP_POINTER_METHOD_AUTO) {
    case MCP_POINTER_METHOD_ABSOLUTE:
        return QEMU_POINTER_ABSOLUTE;
    case MCP_POINTER_METHOD_CLOSED_LOOP:
        return QEMU_POINTER_CLOSED_LOOP;
    case MCP_POINTER_METHOD_OPEN_LOOP:
        return QEMU_POINTER_OPEN_LOOP;
    default:
        return QEMU_POINTER_AUTO;
    }
}

static McpPointer *mcp_pointer_info(QemuConsole *con)
{
    McpPointer *p = g_new0(McpPointer, 1);
    QemuPointerMethod m;
    int x, y;

    if (qemu_pointer_get(con, &x, &y, &m)) {
        p->known = true;
        p->has_x = p->has_y = true;
        p->x = x;
        p->y = y;
        p->method = g_strdup(qemu_pointer_method_str(m));
    }
    return p;
}

static void mcp_pointer_result(McpInputResult *res, QemuConsole *con,
                               const QemuPointerResult *pr, GString *summary)
{
    res->pointer = mcp_pointer_info(con);
    res->has_homed = true;
    res->homed = pr->homed;
    res->has_steps = true;
    res->steps = pr->steps;
    if (pr->step_px) {
        res->has_step_px = true;
        res->step_px = pr->step_px;
    }
    if (pr->calibrated) {
        res->has_calibrated = true;
        res->calibrated = true;
        g_string_append_printf(summary, " (calibrated first: the guest does "
                               "not speed up steps of %d px)", pr->step_px);
    } else if (pr->homed) {
        g_string_append(summary, " (homed first)");
    }
}

McpInputResult *coroutine_fn qmp_mouse_move(bool has_x, int64_t x,
                                            bool has_y, int64_t y,
                                            bool has_dx, int64_t dx,
                                            bool has_dy, int64_t dy,
                                            bool has_method,
                                            McpPointerMethod method,
                                            bool has_home, McpPointerHome home,
                                            bool has_step_px, int64_t step_px,
                                            bool has_step_delay_ms,
                                            int64_t step_delay_ms,
                                            QObject *console, McpThen *then,
                                            Error **errp)
{
    QemuConsole *con = mcp_console(console, 0, errp);
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_ctx(&local);
    g_autoptr(GString) summary = g_string_new(NULL);
    QemuPointerResult pr;
    QemuPointerOpts o;
    McpInputResult *res;
    McpInput in;
    bool ok;

    if (!con) {
        return NULL;
    }
    if ((has_x || has_y) == (has_dx || has_dy) || has_x != has_y) {
        error_setg(errp, "give either x and y, or dx and/or dy");
        return NULL;
    }
    qemu_pointer_opts_init(&o);
    o.method = mcp_method(has_method, method);
    if (has_home) {
        o.home = home == MCP_POINTER_HOME_ALWAYS ? QEMU_POINTER_HOME_ALWAYS :
                 home == MCP_POINTER_HOME_NEVER ? QEMU_POINTER_HOME_NEVER :
                 QEMU_POINTER_HOME_AUTO;
    }
    if (has_step_px) {
        o.step_px = MAX(1, step_px);
        o.step_auto = false;
    }
    if (has_step_delay_ms) {
        o.step_delay_ms = MAX(0, step_delay_ms);
    }
    if (!mcp_input_begin(&in, con, then, ctx, errp)) {
        return NULL;
    }
    if (has_x) {
        ok = qemu_pointer_co_move(con, ctx, x, y, &o, &pr, errp);
    } else {
        ok = qemu_pointer_co_move_by(con, ctx, has_dx ? dx : 0,
                                     has_dy ? dy : 0, &o, &pr, errp);
    }
    if (!ok) {
        mcp_input_abort(&in);
        return NULL;
    }
    res = g_new0(McpInputResult, 1);
    if (pr.known) {
        g_string_printf(summary, "pointer at %d,%d (%s)", pr.x, pr.y,
                        qemu_pointer_method_str(pr.method));
    } else {
        g_string_printf(summary, "moved by %" PRId64 ",%" PRId64
                        "; pointer position unknown",
                        has_dx ? dx : 0, has_dy ? dy : 0);
    }
    mcp_pointer_result(res, con, &pr, summary);
    if (!mcp_co_input_finish(&in, res, summary, errp)) {
        qapi_free_McpInputResult(res);
        return NULL;
    }
    return res;
}

McpInputResult *coroutine_fn qmp_mouse_click(bool has_x, int64_t x,
                                             bool has_y, int64_t y,
                                             bool has_button,
                                             InputButton button,
                                             bool has_count, int64_t count,
                                             bool has_interval_ms,
                                             int64_t interval_ms,
                                             bool has_hold_ms, int64_t hold_ms,
                                             bool has_method,
                                             McpPointerMethod method,
                                             QObject *console, McpThen *then,
                                             Error **errp)
{
    QemuConsole *con = mcp_console(console, 0, errp);
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_ctx(&local);
    g_autoptr(GString) summary = g_string_new(NULL);
    InputButton btn = has_button ? button : INPUT_BUTTON_LEFT;
    QemuPointerResult pr = { 0 };
    QemuInputBatch *b;
    McpInputResult *res;
    McpInput in;
    int i;

    if (!con) {
        return NULL;
    }
    if (has_x != has_y) {
        error_setg(errp, "give both x and y, or neither");
        return NULL;
    }
    if (!has_count) {
        count = 1;
    }
    if (count < 1 || count > 3) {
        error_setg(errp, "count must be 1, 2 or 3");
        return NULL;
    }
    if (!mcp_input_begin(&in, con, then, ctx, errp)) {
        return NULL;
    }
    if (has_x) {
        QemuPointerOpts o;

        qemu_pointer_opts_init(&o);
        o.method = mcp_method(has_method, method);
        if (!qemu_pointer_co_move(con, ctx, x, y, &o, &pr, errp)) {
            mcp_input_abort(&in);
            return NULL;
        }
    }
    b = qemu_input_batch_new(con);
    for (i = 0; i < count; i++) {
        qemu_input_batch_btn(b, btn, true);
        qemu_input_batch_delay(b, has_hold_ms ? hold_ms : 60);
        qemu_input_batch_btn(b, btn, false);
        if (i < count - 1) {
            qemu_input_batch_delay(b, has_interval_ms ? interval_ms : 100);
        }
    }
    if (!qemu_input_co_run(ctx, b, errp)) {
        mcp_input_abort(&in);
        return NULL;
    }
    res = g_new0(McpInputResult, 1);
    g_string_printf(summary, "%s %s-click", count == 1 ? "single" :
                    count == 2 ? "double" : "triple", InputButton_str(btn));
    if (has_x) {
        g_string_append_printf(summary, " at %" PRId64 ",%" PRId64, x, y);
        mcp_pointer_result(res, con, &pr, summary);
    } else {
        res->pointer = mcp_pointer_info(con);
    }
    if (!mcp_co_input_finish(&in, res, summary, errp)) {
        qapi_free_McpInputResult(res);
        return NULL;
    }
    return res;
}

static bool coroutine_fn mcp_co_button(QemuConsole *con, QemuInputCtx *ctx,
                                       InputButton btn, bool down,
                                       int delay_before_ms, Error **errp)
{
    QemuInputBatch *b = qemu_input_batch_new(con);

    qemu_input_batch_delay(b, delay_before_ms);
    qemu_input_batch_btn(b, btn, down);
    return qemu_input_co_run(ctx, b, errp);
}

McpInputResult *coroutine_fn qmp_mouse_drag(McpPoint *from, McpPoint *to,
                                            bool has_button,
                                            InputButton button,
                                            bool has_press_wait_ms,
                                            int64_t press_wait_ms,
                                            bool has_step_px, int64_t step_px,
                                            bool has_step_delay_ms,
                                            int64_t step_delay_ms,
                                            bool has_method,
                                            McpPointerMethod method,
                                            QObject *console, McpThen *then,
                                            Error **errp)
{
    QemuConsole *con = mcp_console(console, 0, errp);
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_ctx(&local);
    g_autoptr(GString) summary = g_string_new(NULL);
    InputButton btn = has_button ? button : INPUT_BUTTON_LEFT;
    int wait = has_press_wait_ms ? press_wait_ms : 150;
    QemuPointerResult pr, pr2;
    QemuPointerOpts o;
    McpInputResult *res;
    McpInput in;

    if (!con) {
        return NULL;
    }
    qemu_pointer_opts_init(&o);
    o.method = mcp_method(has_method, method);
    if (!mcp_input_begin(&in, con, then, ctx, errp)) {
        return NULL;
    }
    if (!qemu_pointer_co_move(con, ctx, from->x, from->y, &o, &pr, errp) ||
        !mcp_co_button(con, ctx, btn, true, 0, errp)) {
        mcp_input_abort(&in);
        return NULL;
    }
    if (has_step_px) {
        o.step_px = MAX(1, step_px);
        o.step_auto = false;
    }
    if (has_step_delay_ms) {
        o.step_delay_ms = MAX(0, step_delay_ms);
    }
    o.home = QEMU_POINTER_HOME_NEVER;
    if (!mcp_co_sleep(ctx, wait) ||
        !qemu_pointer_co_move(con, ctx, to->x, to->y, &o, &pr2, errp) ||
        !mcp_co_button(con, ctx, btn, false, wait, errp)) {
        /* never leave the button down, even for a QMP caller */
        Error *local_err = NULL;
        QemuInputCtx release = { 0 };

        mcp_co_button(con, &release, btn, false, 0, &local_err);
        error_free(local_err);
        mcp_input_abort(&in);
        if (errp && !*errp) {
            error_setg(errp, "cancelled");
        }
        return NULL;
    }
    res = g_new0(McpInputResult, 1);
    g_string_printf(summary, "dragged %s button from %" PRId64 ",%" PRId64
                    " to %" PRId64 ",%" PRId64, InputButton_str(btn),
                    from->x, from->y, to->x, to->y);
    pr.steps += pr2.steps;
    mcp_pointer_result(res, con, &pr, summary);
    if (!mcp_co_input_finish(&in, res, summary, errp)) {
        qapi_free_McpInputResult(res);
        return NULL;
    }
    return res;
}

McpInputResult *coroutine_fn qmp_mouse_button(bool has_button,
                                              InputButton button,
                                              McpButtonAction action,
                                              QObject *console, McpThen *then,
                                              Error **errp)
{
    QemuConsole *con = mcp_console(console, 0, errp);
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_ctx(&local);
    g_autoptr(GString) summary = g_string_new(NULL);
    InputButton btn = has_button ? button : INPUT_BUTTON_LEFT;
    bool down = action == MCP_BUTTON_ACTION_DOWN;
    McpInputResult *res;
    McpInput in;

    if (!con || !mcp_input_begin(&in, con, then, ctx, errp)) {
        return NULL;
    }
    if (!mcp_co_button(con, ctx, btn, down, 0, errp)) {
        mcp_input_abort(&in);
        return NULL;
    }
    res = g_new0(McpInputResult, 1);
    g_string_printf(summary, "%s button %s", InputButton_str(btn),
                    down ? "pressed and held" : "released");
    res->pointer = mcp_pointer_info(con);
    if (!mcp_co_input_finish(&in, res, summary, errp)) {
        qapi_free_McpInputResult(res);
        return NULL;
    }
    return res;
}

/* Text mode */

/* Code page 437, as the PC draws it, control codes included */
static const uint16_t mcp_cp437[256] = {
    0x0020, 0x263a, 0x263b, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    0x25d8, 0x25cb, 0x25d9, 0x2642, 0x2640, 0x266a, 0x266b, 0x263c,
    0x25ba, 0x25c4, 0x2195, 0x203c, 0x00b6, 0x00a7, 0x25ac, 0x21a8,
    0x2191, 0x2193, 0x2192, 0x2190, 0x221f, 0x2194, 0x25b2, 0x25bc,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002a, 0x002b, 0x002c, 0x002d, 0x002e, 0x002f,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003a, 0x003b, 0x003c, 0x003d, 0x003e, 0x003f,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004a, 0x004b, 0x004c, 0x004d, 0x004e, 0x004f,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005a, 0x005b, 0x005c, 0x005d, 0x005e, 0x005f,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006a, 0x006b, 0x006c, 0x006d, 0x006e, 0x006f,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007a, 0x007b, 0x007c, 0x007d, 0x007e, 0x2302,
    0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7,
    0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5,
    0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9,
    0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192,
    0x00e1, 0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba,
    0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
    0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
    0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
    0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
    0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4,
    0x03a6, 0x0398, 0x03a9, 0x03b4, 0x221e, 0x03c6, 0x03b5, 0x2229,
    0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248,
    0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0,
};

#define MCP_TEXT_MAX_CELLS  (160 * 100)
#define MCP_TEXT_HIGHLIGHTS 50

/* Background colour of a console_ch_t cell */
static int mcp_cell_bg(uint32_t ch)
{
    return (ch >> 12) & 7;
}

static char *mcp_cells_text(const uint32_t *cells, int n)
{
    GString *s = g_string_new(NULL);
    int i;

    for (i = 0; i < n; i++) {
        g_string_append_unichar(s, mcp_cp437[cells[i] & 0xff]);
    }
    return g_string_free(s, false);
}

McpScreenTextResult *qmp_screen_text(QObject *console, Error **errp)
{
    QemuConsole *con = mcp_console(console, 0, errp);
    g_autofree uint32_t *cells = NULL;
    g_autoptr(GString) rows = g_string_new(NULL);
    McpTextHighlightList **htail;
    McpScreenTextResult *res;
    QemuFrames *f;
    int w, h, cx, cy, x, y, bg_count[8] = { 0 }, bg = 0, nhl = 0;

    if (!con) {
        return NULL;
    }
    f = qemu_console_frames(con);
    cells = g_new0(uint32_t, MCP_TEXT_MAX_CELLS);
    /* a full update reports the size and cursor and fills every cell */
    qemu_console_hw_invalidate(con);
    qemu_console_hw_text_update(con, cells);
    qemu_frames_get_text(f, &w, &h, &cx, &cy);

    if (!w || !h || w * h > MCP_TEXT_MAX_CELLS) {
        error_setg(errp, "console %d has no text mode; use screenshot",
                   qemu_console_get_index(con));
        return NULL;
    }
    if (w == 60 && h == 3 && cx < 0) {
        /* VGA puts a one-line notice here when not in text mode */
        g_autofree char *msg = mcp_cells_text(cells + w, w);

        if (strstr(msg, "Graphic mode") || strstr(msg, "Blank mode") ||
            strstr(msg, "Text mode")) {
            error_setg(errp, "console %d is not in a text mode it can read "
                       "(%s); use screenshot",
                       qemu_console_get_index(con), g_strstrip(msg));
            return NULL;
        }
    }

    for (x = 0; x < w * h; x++) {
        bg_count[mcp_cell_bg(cells[x])]++;
    }
    for (x = 1; x < 8; x++) {
        if (bg_count[x] > bg_count[bg]) {
            bg = x;
        }
    }

    res = g_new0(McpScreenTextResult, 1);
    res->columns = w;
    res->rows = h;
    if (cx >= 0 && cy >= 0) {
        res->has_cursor_col = res->has_cursor_row = true;
        res->cursor_col = cx;
        res->cursor_row = cy;
    }
    htail = &res->highlights;
    for (y = 0; y < h; y++) {
        const uint32_t *row = cells + y * w;
        g_autofree char *line = mcp_cells_text(row, w);
        int len = strlen(line);

        /* trailing spaces carry nothing */
        while (len && line[len - 1] == ' ') {
            len--;
        }
        g_string_append_len(rows, line, len);
        g_string_append_c(rows, '\n');

        for (x = 0; x < w && nhl < MCP_TEXT_HIGHLIGHTS; x++) {
            McpTextHighlight *hl;
            int start = x;

            if (mcp_cell_bg(row[x]) == bg) {
                continue;
            }
            while (x < w && mcp_cell_bg(row[x]) == mcp_cell_bg(row[start])) {
                x++;
            }
            hl = g_new0(McpTextHighlight, 1);
            hl->row = y;
            hl->col = start;
            hl->len = x - start;
            hl->text = g_strstrip(mcp_cells_text(row + start, x - start));
            QAPI_LIST_APPEND(htail, hl);
            nhl++;
            x--;
        }
    }
    res->summary = g_strdup_printf("console %d, text %dx%d%s:\n%s",
                                   qemu_console_get_index(con), w, h,
                                   cx >= 0 ? "" : ", no cursor", rows->str);
    return res;
}

/* Pointer and wheel */

McpInputResult *coroutine_fn qmp_pointer_locate(QObject *console,
                                                Error **errp)
{
    QemuConsole *con = mcp_console(console, 0, errp);
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_ctx(&local);
    int64_t start = mcp_now_ms();
    QemuPointerResult pr;
    McpInputResult *res;
    const char *how = NULL;

    if (!con) {
        return NULL;
    }
    if (!runstate_is_running()) {
        error_setg(errp, "VM is paused (state %s); resume it first",
                   RunState_str(runstate_get()));
        return NULL;
    }
    if (!qemu_pointer_co_locate(con, ctx, &pr, &how, errp)) {
        return NULL;
    }
    res = g_new0(McpInputResult, 1);
    res->pointer = mcp_pointer_info(con);
    res->elapsed_ms = mcp_now_ms() - start;
    res->summary = g_strdup_printf("pointer at %d,%d (%s)", pr.x, pr.y, how);
    return res;
}

McpInputResult *coroutine_fn qmp_mouse_wheel(bool has_x, int64_t x,
                                             bool has_y, int64_t y,
                                             int64_t steps,
                                             bool has_horizontal,
                                             bool horizontal,
                                             bool has_step_delay_ms,
                                             int64_t step_delay_ms,
                                             bool has_method,
                                             McpPointerMethod method,
                                             QObject *console, McpThen *then,
                                             Error **errp)
{
    QemuConsole *con = mcp_console(console, 0, errp);
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_ctx(&local);
    g_autoptr(GString) summary = g_string_new(NULL);
    QemuPointerResult pr = { 0 };
    InputButton btn;
    QemuInputBatch *b;
    McpInputResult *res;
    McpInput in;
    int i, n = ABS(steps);

    if (!con) {
        return NULL;
    }
    if (has_x != has_y) {
        error_setg(errp, "give both x and y, or neither");
        return NULL;
    }
    if (n < 1 || n > 1000) {
        error_setg(errp, "steps must be from 1 to 1000, or -1 to -1000");
        return NULL;
    }
    if (horizontal) {
        btn = steps > 0 ? INPUT_BUTTON_WHEEL_RIGHT : INPUT_BUTTON_WHEEL_LEFT;
    } else {
        btn = steps > 0 ? INPUT_BUTTON_WHEEL_DOWN : INPUT_BUTTON_WHEEL_UP;
    }
    if (!mcp_input_begin(&in, con, then, ctx, errp)) {
        return NULL;
    }
    if (has_x) {
        QemuPointerOpts o;

        qemu_pointer_opts_init(&o);
        o.method = mcp_method(has_method, method);
        if (!qemu_pointer_co_move(con, ctx, x, y, &o, &pr, errp)) {
            mcp_input_abort(&in);
            return NULL;
        }
    }
    b = qemu_input_batch_new(con);
    for (i = 0; i < n; i++) {
        qemu_input_batch_btn(b, btn, true);
        qemu_input_batch_btn(b, btn, false);
        qemu_input_batch_delay(b, has_step_delay_ms ? step_delay_ms : 50);
    }
    if (!qemu_input_co_run(ctx, b, errp)) {
        mcp_input_abort(&in);
        return NULL;
    }
    res = g_new0(McpInputResult, 1);
    g_string_printf(summary, "wheel %s %d step%s", InputButton_str(btn) + 6,
                    n, n == 1 ? "" : "s");
    if (has_x) {
        g_string_append_printf(summary, " at %" PRId64 ",%" PRId64, x, y);
        mcp_pointer_result(res, con, &pr, summary);
    } else {
        res->pointer = mcp_pointer_info(con);
    }
    if (!mcp_co_input_finish(&in, res, summary, errp)) {
        qapi_free_McpInputResult(res);
        return NULL;
    }
    return res;
}

/* Frames */

#define MCP_FRAMES_MAX      100
#define MCP_THUMBS_MAX      5

McpFramesResult *qmp_frames(QObject *console, bool has_count, int64_t count,
                            bool has_thumbnails, int64_t thumbnails,
                            Error **errp)
{
    QemuConsole *con = mcp_console(console, 0, errp);
    g_autofree QemuFramesInfo *info = NULL;
    g_autoptr(GString) summary = g_string_new(NULL);
    McpFrameInfoList **tail;
    McpImageList **itail;
    McpFramesResult *res;
    int n, i, nthumbs;

    if (!con) {
        return NULL;
    }
    count = has_count ? MIN(MAX(count, 1), MCP_FRAMES_MAX) : 20;
    nthumbs = has_thumbnails ? MIN(MAX(thumbnails, 0), MCP_THUMBS_MAX) : 0;
    info = g_new0(QemuFramesInfo, count);
    n = qemu_frames_history(qemu_console_frames(con), info, count);

    res = g_new0(McpFramesResult, 1);
    tail = &res->frames;
    itail = &res->images;
    g_string_printf(summary, "%d frame%s, newest first:", n,
                    n == 1 ? "" : "s");
    for (i = 0; i < n; i++) {
        McpFrameInfo *fi = g_new0(McpFrameInfo, 1);

        fi->frame = info[i].frame;
        fi->ago_ms = info[i].ago_ms;
        fi->shown_ms = info[i].shown_ms;
        fi->current = info[i].current;
        g_string_append_printf(summary, "\n%" PRIu64 ": %s%" PRId64
                               " ms ago, shown %" PRId64 " ms",
                               fi->frame, fi->current ? "now, since " : "",
                               fi->ago_ms, fi->shown_ms);
        if (!info[i].current) {
            fi->box = g_new0(ScreenRegion, 1);
            fi->box->x = info[i].box.x;
            fi->box->y = info[i].box.y;
            fi->box->w = info[i].box.w;
            fi->box->h = info[i].box.h;
            fi->has_changed_pixels = true;
            fi->changed_pixels = info[i].pixels;
            g_string_append_printf(summary, ", then %d pixels changed in "
                                   "%d,%d %dx%d", info[i].pixels,
                                   info[i].box.x, info[i].box.y,
                                   info[i].box.w, info[i].box.h);
        }
        if (info[i].w) {
            fi->has_width = fi->has_height = true;
            fi->width = info[i].w;
            fi->height = info[i].h;
            g_string_append_printf(summary, " (%dx%d)", info[i].w,
                                   info[i].h);
        }
        QAPI_LIST_APPEND(tail, fi);
        if (i < nthumbs) {
            McpImage *mi = mcp_frame_image(con, info[i].frame, NULL, 0.25,
                                           errp);

            if (!mi) {
                qapi_free_McpFramesResult(res);
                return NULL;
            }
            QAPI_LIST_APPEND(itail, mi);
            res->has_images = true;
        }
    }
    res->summary = g_strdup(summary->str);
    return res;
}
