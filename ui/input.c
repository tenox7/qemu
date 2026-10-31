#include "qemu/osdep.h"
#include "system/system.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-ui.h"
#include "trace.h"
#include "ui/input.h"
#include "ui/console.h"
#include "qemu/host-utils.h"
#include "system/replay.h"
#include "system/runstate.h"

struct QemuInputHandlerState {
    DeviceState       *dev;
    const QemuInputHandler *handler;
    int               id;
    int               events;
    QemuConsole       *con;
    uint32_t          leds_mask;
    QTAILQ_ENTRY(QemuInputHandlerState) node;
};

/*
 * The input scheduler.
 *
 * Events wait in lanes, one per target console (NULL is the default
 * lane).  A lane delivers its items in order; DELAY items pause it on
 * QEMU_CLOCK_VIRTUAL, so pacing follows the guest's own time and stops
 * while the VM is paused.  Items belong to a batch; the END item of a
 * batch calls its completion callback.  There is no fixed cap: only
 * absurd batch sizes are refused.
 *
 * Every key and button that a batch presses is remembered until it is
 * released, so a cancelled batch or a departing owner (for example an
 * MCP client that disconnects) never leaves a key or button down.
 */
typedef enum {
    QEMU_INPUT_ITEM_DELAY = 1,
    QEMU_INPUT_ITEM_EVENT,
    QEMU_INPUT_ITEM_SYNC,
    QEMU_INPUT_ITEM_END,
} QemuInputItemType;

typedef struct QemuInputItem {
    QemuInputItemType type;
    uint64_t batch;             /* 0 for the legacy send-key interface */
    uint32_t delay_ms;
    QemuConsole *src;
    QemuInputEvent evt;
    QSIMPLEQ_ENTRY(QemuInputItem) node;
} QemuInputItem;

typedef QSIMPLEQ_HEAD(, QemuInputItem) QemuInputItemList;

struct QemuInputBatch {
    QemuConsole *con;
    QemuInputItemList items;
    size_t count;
};

typedef struct QemuInputPending {
    uint64_t id;
    void *owner;
    QemuInputBatchDone *done;
    void *opaque;
    QTAILQ_ENTRY(QemuInputPending) node;
} QemuInputPending;

typedef struct QemuInputLane {
    QemuConsole *con;
    QemuInputItemList items;
    QEMUTimer *timer;
    bool waiting;               /* the head DELAY item is running */
    QTAILQ_ENTRY(QemuInputLane) node;
} QemuInputLane;

/* A key or button that the scheduler pressed and has not released */
typedef struct QemuInputHeld {
    uint64_t batch;
    void *owner;
    QemuConsole *src;
    bool is_button;
    unsigned int code;          /* Linux key code or InputButton */
    QTAILQ_ENTRY(QemuInputHeld) node;
} QemuInputHeld;

#define QEMU_INPUT_BATCH_MAX (1024 * 1024)

static QTAILQ_HEAD(, QemuInputLane) input_lanes =
    QTAILQ_HEAD_INITIALIZER(input_lanes);
static QTAILQ_HEAD(, QemuInputPending) input_pending =
    QTAILQ_HEAD_INITIALIZER(input_pending);
static QTAILQ_HEAD(, QemuInputHeld) input_held =
    QTAILQ_HEAD_INITIALIZER(input_held);
static uint64_t input_next_batch = 1;
static uint32_t kbd_default_delay_ms = 10;
static int input_scheduler_delivering;
static uint64_t input_foreign_mouse;

static QTAILQ_HEAD(, QemuInputHandlerState) handlers =
    QTAILQ_HEAD_INITIALIZER(handlers);
static NotifierList mouse_mode_notifiers =
    NOTIFIER_LIST_INITIALIZER(mouse_mode_notifiers);
static NotifierList leds_notifiers =
    NOTIFIER_LIST_INITIALIZER(leds_notifiers);


static void notify_input_changed(uint32_t mask)
{
    notifier_list_notify(&mouse_mode_notifiers, NULL);
    if (mask & INPUT_EVENT_MASK_KEY) {
        notifier_list_notify(&leds_notifiers, NULL);
    }
}

QemuInputHandlerState *qemu_input_handler_register(DeviceState *dev,
                                            const QemuInputHandler *handler)
{
    QemuInputHandlerState *s = g_new0(QemuInputHandlerState, 1);
    static int id = 1;

    s->dev = dev;
    s->handler = handler;
    s->id = id++;
    QTAILQ_INSERT_TAIL(&handlers, s, node);
    notify_input_changed(handler->mask);

    return s;
}

void qemu_input_handler_activate(QemuInputHandlerState *s)
{
    QTAILQ_REMOVE(&handlers, s, node);
    QTAILQ_INSERT_HEAD(&handlers, s, node);
    notify_input_changed(s->handler->mask);
}

void qemu_input_handler_deactivate(QemuInputHandlerState *s)
{
    QTAILQ_REMOVE(&handlers, s, node);
    QTAILQ_INSERT_TAIL(&handlers, s, node);
    notify_input_changed(s->handler->mask);
}

void qemu_input_handler_unregister(QemuInputHandlerState *s)
{
    uint32_t mask = s->handler->mask;

    QTAILQ_REMOVE(&handlers, s, node);
    g_free(s);
    notify_input_changed(mask);
}

void qemu_input_handler_bind(QemuInputHandlerState *s,
                             const char *device_id, int head,
                             Error **errp)
{
    QemuConsole *con;
    Error *err = NULL;

    con = qemu_console_lookup_by_device_name(device_id, head, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    s->con = con;
}

static QemuInputHandlerState*
qemu_input_find_handler(uint32_t mask, const QemuConsole *con)
{
    QemuInputHandlerState *s;

    QTAILQ_FOREACH(s, &handlers, node) {
        if (s->con == NULL || s->con != con) {
            continue;
        }
        if (mask & s->handler->mask) {
            return s;
        }
    }

    QTAILQ_FOREACH(s, &handlers, node) {
        if (s->con != NULL) {
            continue;
        }
        if (mask & s->handler->mask) {
            return s;
        }
    }
    return NULL;
}

void qemu_input_handler_set_leds_mask(QemuInputHandlerState *s, uint32_t leds_mask)
{
    assert(s->handler->mask & INPUT_EVENT_MASK_KEY);
    s->leds_mask = leds_mask;
    notifier_list_notify(&leds_notifiers, NULL);
}

void qemu_input_led_notifier_add(Notifier *n)
{
    notifier_list_add(&leds_notifiers, n);
}

void qemu_input_led_notifier_remove(Notifier *n)
{
    notifier_remove(n);
}

void qmp_input_send_event(const char *device,
                          bool has_head, int64_t head,
                          InputEventList *events, Error **errp)
{
    InputEventList *e;
    QemuConsole *con;
    Error *err = NULL;

    con = NULL;
    if (device) {
        if (!has_head) {
            head = 0;
        }
        con = qemu_console_lookup_by_device_name(device, head, &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }
    }

    if (!runstate_is_running() && !runstate_check(RUN_STATE_SUSPENDED)) {
        error_setg(errp, "VM not running");
        return;
    }

    for (e = events; e != NULL; e = e->next) {
        InputEvent *event = e->value;

        if (!qemu_input_find_handler(1 << event->type, con)) {
            error_setg(errp, "Input handler not found for "
                             "event type %s",
                            InputEventKind_str(event->type));
            return;
        }
    }

    for (e = events; e != NULL; e = e->next) {
        InputEvent *qapi = e->value;
        QemuInputEvent evt;

        evt.type = qapi->type;

        switch (qapi->type) {
        case INPUT_EVENT_KIND_KEY:
            evt.key.key = qemu_input_key_value_to_linux(qapi->u.key.data->key);
            evt.key.down = qapi->u.key.data->down;
            break;

        case INPUT_EVENT_KIND_BTN:
            evt.btn = *qapi->u.btn.data;
            break;

        case INPUT_EVENT_KIND_REL:
            evt.rel = *qapi->u.rel.data;
            break;

        case INPUT_EVENT_KIND_ABS:
            evt.abs = *qapi->u.abs.data;
            break;

        case INPUT_EVENT_KIND_MTT:
            evt.mtt = *qapi->u.mtt.data;
            break;

        default:
            g_assert_not_reached();
        }

        qemu_input_event_send(con, &evt);
    }

    qemu_input_event_sync();
}

static void qemu_input_event_trace(QemuConsole *src, QemuInputEvent *evt)
{
    const char *name;
    int idx = -1;
    QemuInputKeyEvent *key;
    InputBtnEvent *btn;
    InputMoveEvent *move;
    InputMultiTouchEvent *mtt;

    if (src) {
        idx = qemu_console_get_index(src);
    }
    switch (evt->type) {
    case INPUT_EVENT_KIND_KEY:
        key = &evt->key;
        name = QKeyCode_str(qemu_input_linux_to_qcode(key->key));
        trace_input_event_key_qcode(idx, name, key->down);
        break;
    case INPUT_EVENT_KIND_BTN:
        btn = &evt->btn;
        name = btn->button < INPUT_BUTTON__MAX ? InputButton_str(btn->button) : "invalid";
        trace_input_event_btn(idx, name, btn->down);
        break;
    case INPUT_EVENT_KIND_REL:
        move = &evt->rel;
        name = move->axis < INPUT_AXIS__MAX ? InputAxis_str(move->axis) : "invalid";
        trace_input_event_rel(idx, name, move->value);
        break;
    case INPUT_EVENT_KIND_ABS:
        move = &evt->abs;
        name = move->axis < INPUT_AXIS__MAX ? InputAxis_str(move->axis) : "invalid";
        trace_input_event_abs(idx, name, move->value);
        break;
    case INPUT_EVENT_KIND_MTT:
        mtt = &evt->mtt;
        name = mtt->axis < INPUT_AXIS__MAX ? InputAxis_str(mtt->axis) : "invalid";
        trace_input_event_mtt(idx, name, mtt->value);
        break;
    case INPUT_EVENT_KIND__MAX:
        /* keep gcc happy */
        break;
    }
}

static void qemu_input_lane_run(QemuInputLane *lane);

static void qemu_input_lane_timer(void *opaque)
{
    QemuInputLane *lane = opaque;
    QemuInputItem *item = QSIMPLEQ_FIRST(&lane->items);

    g_assert(lane->waiting && item && item->type == QEMU_INPUT_ITEM_DELAY);
    lane->waiting = false;
    QSIMPLEQ_REMOVE_HEAD(&lane->items, node);
    g_free(item);
    qemu_input_lane_run(lane);
}

static void qemu_input_vm_state(void *opaque, bool running, RunState state)
{
    QemuInputLane *lane;

    if (!running) {
        return;
    }
    QTAILQ_FOREACH(lane, &input_lanes, node) {
        qemu_input_lane_run(lane);
    }
}

static QemuInputLane *qemu_input_lane_get(QemuConsole *con)
{
    static VMChangeStateEntry *vm_state;
    QemuInputLane *lane;

    if (!vm_state) {
        vm_state = qemu_add_vm_change_state_handler(qemu_input_vm_state,
                                                    NULL);
    }

    QTAILQ_FOREACH(lane, &input_lanes, node) {
        if (lane->con == con) {
            return lane;
        }
    }
    lane = g_new0(QemuInputLane, 1);
    lane->con = con;
    QSIMPLEQ_INIT(&lane->items);
    lane->timer = timer_new_full(NULL, QEMU_CLOCK_VIRTUAL, SCALE_MS,
                                 QEMU_TIMER_ATTR_EXTERNAL,
                                 qemu_input_lane_timer, lane);
    QTAILQ_INSERT_TAIL(&input_lanes, lane, node);
    return lane;
}

static QemuInputPending *qemu_input_pending_find(uint64_t id)
{
    QemuInputPending *p;

    QTAILQ_FOREACH(p, &input_pending, node) {
        if (p->id == id) {
            return p;
        }
    }
    return NULL;
}

static void qemu_input_held_track(uint64_t batch, QemuConsole *src,
                                  QemuInputEvent *evt)
{
    QemuInputPending *p = qemu_input_pending_find(batch);
    QemuInputHeld *h, *next;
    bool is_button;
    unsigned int code;
    bool down;

    if (evt->type == INPUT_EVENT_KIND_KEY) {
        is_button = false;
        code = evt->key.key;
        down = evt->key.down;
    } else if (evt->type == INPUT_EVENT_KIND_BTN) {
        is_button = true;
        code = evt->btn.button;
        down = evt->btn.down;
    } else {
        return;
    }

    QTAILQ_FOREACH_SAFE(h, &input_held, node, next) {
        if (h->src == src && h->is_button == is_button && h->code == code) {
            QTAILQ_REMOVE(&input_held, h, node);
            g_free(h);
        }
    }
    if (down) {
        h = g_new0(QemuInputHeld, 1);
        h->batch = batch;
        h->owner = p ? p->owner : NULL;
        h->src = src;
        h->is_button = is_button;
        h->code = code;
        QTAILQ_INSERT_TAIL(&input_held, h, node);
    }
}

static void qemu_input_deliver(uint64_t batch, QemuConsole *src,
                               QemuInputEvent *evt)
{
    if (batch) {
        qemu_input_held_track(batch, src, evt);
    }
    input_scheduler_delivering++;
    qemu_input_event_send(src, evt);
    input_scheduler_delivering--;
}

static void qemu_input_batch_finish(uint64_t id, bool cancelled)
{
    QemuInputPending *p = qemu_input_pending_find(id);

    if (p) {
        QTAILQ_REMOVE(&input_pending, p, node);
        if (p->done) {
            p->done(p->opaque, cancelled);
        }
        g_free(p);
    }
}

static bool qemu_input_vm_takes_input(void)
{
    return runstate_is_running() || runstate_check(RUN_STATE_SUSPENDED);
}

static void qemu_input_lane_run(QemuInputLane *lane)
{
    QemuInputItem *item;

    while (!lane->waiting && (item = QSIMPLEQ_FIRST(&lane->items))) {
        /*
         * Hold input while the VM is paused; the guest would drop it.
         * The lanes start again when it resumes.
         */
        if (item->type != QEMU_INPUT_ITEM_DELAY &&
            item->type != QEMU_INPUT_ITEM_END &&
            !qemu_input_vm_takes_input()) {
            return;
        }
        switch (item->type) {
        case QEMU_INPUT_ITEM_DELAY:
            lane->waiting = true;
            timer_mod(lane->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)
                      + item->delay_ms);
            return;
        case QEMU_INPUT_ITEM_EVENT:
            qemu_input_deliver(item->batch, item->src, &item->evt);
            break;
        case QEMU_INPUT_ITEM_SYNC:
            input_scheduler_delivering++;
            qemu_input_event_sync();
            input_scheduler_delivering--;
            break;
        case QEMU_INPUT_ITEM_END:
            break;
        }
        QSIMPLEQ_REMOVE_HEAD(&lane->items, node);
        if (item->type == QEMU_INPUT_ITEM_END) {
            uint64_t id = item->batch;

            g_free(item);
            /* the callback may queue more input; it starts the lane again */
            qemu_input_batch_finish(id, false);
            continue;
        }
        g_free(item);
    }
}

static QemuInputItem *qemu_input_item_new(QemuInputItemType type)
{
    QemuInputItem *item = g_new0(QemuInputItem, 1);

    item->type = type;
    return item;
}

static void qemu_input_legacy_append(QemuInputItem *item)
{
    QemuInputLane *lane = qemu_input_lane_get(NULL);

    QSIMPLEQ_INSERT_TAIL(&lane->items, item, node);
    qemu_input_lane_run(lane);
}

QemuInputBatch *qemu_input_batch_new(QemuConsole *con)
{
    QemuInputBatch *b = g_new0(QemuInputBatch, 1);

    b->con = con;
    QSIMPLEQ_INIT(&b->items);
    return b;
}

void qemu_input_batch_free(QemuInputBatch *b)
{
    QemuInputItem *item;

    if (!b) {
        return;
    }
    while ((item = QSIMPLEQ_FIRST(&b->items))) {
        QSIMPLEQ_REMOVE_HEAD(&b->items, node);
        g_free(item);
    }
    g_free(b);
}

static void qemu_input_batch_append(QemuInputBatch *b, QemuInputItem *item)
{
    QSIMPLEQ_INSERT_TAIL(&b->items, item, node);
    b->count++;
}

void qemu_input_batch_event(QemuInputBatch *b, QemuInputEvent *evt)
{
    QemuInputItem *item = qemu_input_item_new(QEMU_INPUT_ITEM_EVENT);

    item->src = b->con;
    item->evt = *evt;
    qemu_input_batch_append(b, item);
}

void qemu_input_batch_sync(QemuInputBatch *b)
{
    qemu_input_batch_append(b, qemu_input_item_new(QEMU_INPUT_ITEM_SYNC));
}

void qemu_input_batch_delay(QemuInputBatch *b, uint32_t delay_ms)
{
    QemuInputItem *item;

    if (!delay_ms) {
        return;
    }
    item = qemu_input_item_new(QEMU_INPUT_ITEM_DELAY);
    item->delay_ms = delay_ms;
    qemu_input_batch_append(b, item);
}

void qemu_input_batch_key(QemuInputBatch *b, unsigned int lnx, bool down)
{
    QemuInputEvent evt = {
        .type = INPUT_EVENT_KIND_KEY,
        .key = { .key = lnx, .down = down },
    };

    qemu_input_batch_event(b, &evt);
    qemu_input_batch_sync(b);
}

void qemu_input_batch_btn(QemuInputBatch *b, InputButton btn, bool down)
{
    QemuInputEvent evt = {
        .type = INPUT_EVENT_KIND_BTN,
        .btn = { .button = btn, .down = down },
    };

    qemu_input_batch_event(b, &evt);
    qemu_input_batch_sync(b);
}

void qemu_input_batch_move(QemuInputBatch *b, InputEventKind kind,
                           int x, int y)
{
    QemuInputEvent evt = { .type = kind };

    evt.rel.axis = INPUT_AXIS_X;
    evt.rel.value = x;
    qemu_input_batch_event(b, &evt);
    evt.rel.axis = INPUT_AXIS_Y;
    evt.rel.value = y;
    qemu_input_batch_event(b, &evt);
    qemu_input_batch_sync(b);
}

size_t qemu_input_batch_length(QemuInputBatch *b)
{
    return b->count;
}

uint64_t qemu_input_batch_submit(QemuInputBatch *b, void *owner,
                                 QemuInputBatchDone *done, void *opaque,
                                 Error **errp)
{
    QemuInputLane *lane;
    QemuInputPending *p;
    QemuInputItem *item;
    uint64_t id;

    if (b->count > QEMU_INPUT_BATCH_MAX) {
        error_setg(errp, "too much input at once (%zu events, at most %d)",
                   b->count, QEMU_INPUT_BATCH_MAX);
        qemu_input_batch_free(b);
        return 0;
    }

    id = input_next_batch++;
    p = g_new0(QemuInputPending, 1);
    p->id = id;
    p->owner = owner;
    p->done = done;
    p->opaque = opaque;
    QTAILQ_INSERT_TAIL(&input_pending, p, node);

    lane = qemu_input_lane_get(b->con);
    while ((item = QSIMPLEQ_FIRST(&b->items))) {
        QSIMPLEQ_REMOVE_HEAD(&b->items, node);
        item->batch = id;
        QSIMPLEQ_INSERT_TAIL(&lane->items, item, node);
    }
    item = qemu_input_item_new(QEMU_INPUT_ITEM_END);
    item->batch = id;
    QSIMPLEQ_INSERT_TAIL(&lane->items, item, node);
    g_free(b);

    qemu_input_lane_run(lane);
    return id;
}

/* Release what @match pressed; @match NULL releases everything */
static void qemu_input_release(bool (*match)(QemuInputHeld *, void *),
                               void *opaque)
{
    QemuInputHeld *h, *next;
    QemuInputEvent evt;
    bool any = false;

    QTAILQ_FOREACH_SAFE(h, &input_held, node, next) {
        if (!match(h, opaque)) {
            continue;
        }
        QTAILQ_REMOVE(&input_held, h, node);
        if (h->is_button) {
            evt.type = INPUT_EVENT_KIND_BTN;
            evt.btn.button = h->code;
            evt.btn.down = false;
        } else {
            evt.type = INPUT_EVENT_KIND_KEY;
            evt.key.key = h->code;
            evt.key.down = false;
        }
        qemu_input_deliver(0, h->src, &evt);
        any = true;
        g_free(h);
    }
    if (any) {
        input_scheduler_delivering++;
        qemu_input_event_sync();
        input_scheduler_delivering--;
    }
}

static bool qemu_input_held_by_batch(QemuInputHeld *h, void *opaque)
{
    return h->batch == *(uint64_t *)opaque;
}

static bool qemu_input_held_by_owner(QemuInputHeld *h, void *opaque)
{
    return h->owner == opaque;
}

/* Drop the queued items of batch @id from its lane */
static void qemu_input_batch_unqueue(uint64_t id)
{
    QemuInputLane *lane;
    QemuInputItem *item, *next;

    QTAILQ_FOREACH(lane, &input_lanes, node) {
        bool head_removed = false;

        QSIMPLEQ_FOREACH_SAFE(item, &lane->items, node, next) {
            if (item->batch != id) {
                continue;
            }
            if (item == QSIMPLEQ_FIRST(&lane->items)) {
                head_removed = true;
            }
            QSIMPLEQ_REMOVE(&lane->items, item, QemuInputItem, node);
            g_free(item);
        }
        if (head_removed && lane->waiting) {
            /* the delay that was running belonged to this batch */
            timer_del(lane->timer);
            lane->waiting = false;
            qemu_input_lane_run(lane);
        }
    }
}

void qemu_input_batch_cancel(uint64_t id)
{
    if (!qemu_input_pending_find(id)) {
        return;
    }
    qemu_input_batch_unqueue(id);
    qemu_input_release(qemu_input_held_by_batch, &id);
    qemu_input_batch_finish(id, true);
}

void qemu_input_release_owner(void *owner)
{
    QemuInputPending *p, *next;

    QTAILQ_FOREACH_SAFE(p, &input_pending, node, next) {
        if (p->owner == owner) {
            qemu_input_batch_cancel(p->id);
        }
    }
    qemu_input_release(qemu_input_held_by_owner, owner);
}

bool qemu_input_scheduler_delivering(void)
{
    return input_scheduler_delivering > 0;
}

uint64_t qemu_input_foreign_mouse_count(void)
{
    return input_foreign_mouse;
}

typedef struct QemuInputRun {
    QemuInputCtx *ctx;
    bool done;
    bool cancelled;
} QemuInputRun;

static void qemu_input_run_done(void *opaque, bool cancelled)
{
    QemuInputRun *run = opaque;

    run->done = true;
    run->cancelled = cancelled;
    run->ctx->batch = 0;
    qemu_co_sleep_wake(&run->ctx->sleep);
}

bool coroutine_fn qemu_input_co_run(QemuInputCtx *ctx, QemuInputBatch *b,
                                    Error **errp)
{
    QemuInputRun run = { .ctx = ctx };
    uint64_t id;

    if (ctx->cancelled) {
        qemu_input_batch_free(b);
        error_setg(errp, "cancelled");
        return false;
    }
    id = qemu_input_batch_submit(b, ctx->owner, qemu_input_run_done, &run,
                                 errp);
    if (!id) {
        return false;
    }
    if (!run.done) {
        ctx->batch = id;
    }
    while (!run.done) {
        qemu_co_sleep(&ctx->sleep);
    }
    if (run.cancelled || ctx->cancelled) {
        error_setg(errp, "cancelled");
        return false;
    }
    return true;
}

static QLIST_HEAD(, QemuInputCtx) input_ctxs =
    QLIST_HEAD_INITIALIZER(input_ctxs);

void qemu_input_ctx_init(QemuInputCtx *ctx, void *owner)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->owner = owner;
    ctx->active = true;
    QLIST_INSERT_HEAD(&input_ctxs, ctx, node);
}

void qemu_input_ctx_end(QemuInputCtx *ctx)
{
    if (ctx->active) {
        QLIST_REMOVE(ctx, node);
        ctx->active = false;
    }
}

void qemu_input_cancel_all(void)
{
    QemuInputPending *p, *next;
    QemuInputCtx *ctx;

    QLIST_FOREACH(ctx, &input_ctxs, node) {
        qemu_input_ctx_cancel(ctx);
    }
    QTAILQ_FOREACH_SAFE(p, &input_pending, node, next) {
        qemu_input_batch_cancel(p->id);
    }
}

void qemu_input_ctx_cancel(QemuInputCtx *ctx)
{
    ctx->cancelled = true;
    if (ctx->batch) {
        qemu_input_batch_cancel(ctx->batch);
    }
    qemu_co_sleep_wake(&ctx->sleep);
}

void qemu_input_event_send_impl(QemuConsole *src, QemuInputEvent *evt)
{
    QemuInputHandlerState *s;

    qemu_input_event_trace(src, evt);

    /* send event */
    s = qemu_input_find_handler(1 << evt->type, src);
    if (!s) {
        return;
    }
    s->handler->event(s->dev, src, evt);
    s->events++;
}

void qemu_input_event_send(QemuConsole *src, QemuInputEvent *evt)
{
    if (evt->type == INPUT_EVENT_KIND_KEY && !evt->key.key) {
        return;
    }

    if (!runstate_is_running() && !runstate_check(RUN_STATE_SUSPENDED)) {
        return;
    }

    if (!input_scheduler_delivering && evt->type != INPUT_EVENT_KIND_KEY) {
        input_foreign_mouse++;
    }
    replay_input_event(src, evt);
}

void qemu_input_event_sync_impl(void)
{
    QemuInputHandlerState *s;

    trace_input_event_sync();

    QTAILQ_FOREACH(s, &handlers, node) {
        if (!s->events) {
            continue;
        }
        if (s->handler->sync) {
            s->handler->sync(s->dev);
        }
        s->events = 0;
    }
}

void qemu_input_event_sync(void)
{
    if (!runstate_is_running() && !runstate_check(RUN_STATE_SUSPENDED)) {
        return;
    }

    replay_input_sync_event();
}

void qemu_input_event_send_key_linux(QemuConsole *src, unsigned int lnx,
                                     bool down)
{
    QemuInputItem *item = qemu_input_item_new(QEMU_INPUT_ITEM_EVENT);

    item->src = src;
    item->evt.type = INPUT_EVENT_KIND_KEY;
    item->evt.key.key = lnx;
    item->evt.key.down = down;
    qemu_input_legacy_append(item);
    qemu_input_legacy_append(qemu_input_item_new(QEMU_INPUT_ITEM_SYNC));
}

void qemu_input_event_send_key_number(QemuConsole *src, int num, bool down)
{
    unsigned int lnx = qemu_input_key_number_to_linux(num);
    qemu_input_event_send_key_linux(src, lnx, down);
}

void qemu_input_event_send_key_delay(uint32_t delay_ms)
{
    QemuInputItem *item;

    if (!runstate_is_running() && !runstate_check(RUN_STATE_SUSPENDED)) {
        return;
    }

    item = qemu_input_item_new(QEMU_INPUT_ITEM_DELAY);
    item->delay_ms = delay_ms ? delay_ms : kbd_default_delay_ms;
    qemu_input_legacy_append(item);
}

void qemu_input_queue_btn(QemuConsole *src, InputButton btn, bool down)
{
    QemuInputEvent evt = {
        .type = INPUT_EVENT_KIND_BTN,
        .btn = {
            .button = btn,
            .down = down,
        }
    };

    qemu_input_event_send(src, &evt);
}

void qemu_input_update_buttons(QemuConsole *src, uint32_t *button_map,
                               uint32_t button_old, uint32_t button_new)
{
    InputButton btn;
    uint32_t mask;

    for (btn = 0; btn < INPUT_BUTTON__MAX; btn++) {
        mask = button_map[btn];
        if ((button_old & mask) == (button_new & mask)) {
            continue;
        }
        qemu_input_queue_btn(src, btn, button_new & mask);
    }
}

bool qemu_input_is_absolute(const QemuConsole *con)
{
    QemuInputHandlerState *s;

    s = qemu_input_find_handler(INPUT_EVENT_MASK_REL | INPUT_EVENT_MASK_ABS,
                                con);
    return (s != NULL) && (s->handler->mask & INPUT_EVENT_MASK_ABS);
}

const char *qemu_input_mouse_kind(const QemuConsole *con)
{
    QemuInputHandlerState *s;

    s = qemu_input_find_handler(INPUT_EVENT_MASK_REL | INPUT_EVENT_MASK_ABS,
                                con);
    if (!s) {
        return "none";
    }
    return (s->handler->mask & INPUT_EVENT_MASK_ABS) ? "absolute" : "relative";
}

uint32_t qemu_input_get_leds_mask(const QemuConsole *con)
{
    QemuInputHandlerState *s;

    s = qemu_input_find_handler(INPUT_EVENT_MASK_KEY, con);
    if (s) {
        return s->leds_mask;
    }
    return 0;
}

int qemu_input_scale_axis(int value,
                          int min_in, int max_in,
                          int min_out, int max_out)
{
    int64_t range_in = (int64_t)max_in - min_in;
    int64_t range_out = (int64_t)max_out - min_out;
    int64_t diff = (int64_t)value - min_in;
    bool neg;
    int64_t scaled;

    if (range_in < 1) {
        return min_out + range_out / 2;
    }
    neg = (diff < 0) != (range_out < 0);
    scaled = muldiv64(llabs(diff), llabs(range_out), llabs(range_in));
    return (neg ? -scaled : scaled) + min_out;
}

void qemu_input_queue_rel(QemuConsole *src, InputAxis axis, int value)
{
    QemuInputEvent evt = {
        .type = INPUT_EVENT_KIND_REL,
        .rel = {
            .axis = axis,
            .value = value,
        },
    };

    qemu_input_event_send(src, &evt);
}

void qemu_input_queue_abs(QemuConsole *src, InputAxis axis, int value,
                          int min_in, int max_in)
{
    QemuInputEvent evt = {
        .type = INPUT_EVENT_KIND_ABS,
        .abs = {
            .axis = axis,
            .value = qemu_input_scale_axis(value, min_in, max_in,
                                           INPUT_EVENT_ABS_MIN,
                                           INPUT_EVENT_ABS_MAX),
        },
    };

    qemu_input_event_send(src, &evt);
}

void qemu_input_queue_mtt(QemuConsole *src, InputMultiTouchType type,
                          int slot, int tracking_id)
{
    QemuInputEvent evt = {
        .type = INPUT_EVENT_KIND_MTT,
        .mtt = {
            .type = type,
            .slot = slot,
            .tracking_id = tracking_id,
        },
    };

    qemu_input_event_send(src, &evt);
}

void qemu_input_queue_mtt_abs(QemuConsole *src, InputAxis axis, int value,
                              int min_in, int max_in, int slot, int tracking_id)
{
    QemuInputEvent evt = {
        .type = INPUT_EVENT_KIND_MTT,
        .mtt = {
            .type = INPUT_MULTI_TOUCH_TYPE_DATA,
            .slot = slot,
            .tracking_id = tracking_id,
            .axis = axis,
            .value = qemu_input_scale_axis(value, min_in, max_in,
                                           INPUT_EVENT_ABS_MIN,
                                           INPUT_EVENT_ABS_MAX),
        }
    };

    qemu_input_event_send(src, &evt);
}

void qemu_add_mouse_mode_change_notifier(Notifier *notify)
{
    notifier_list_add(&mouse_mode_notifiers, notify);
}

void qemu_remove_mouse_mode_change_notifier(Notifier *notify)
{
    notifier_remove(notify);
}

MouseInfoList *qmp_query_mice(Error **errp)
{
    MouseInfoList *mice_list = NULL;
    MouseInfo *info;
    QemuInputHandlerState *s;
    bool current = true;

    QTAILQ_FOREACH(s, &handlers, node) {
        if (!(s->handler->mask &
              (INPUT_EVENT_MASK_REL | INPUT_EVENT_MASK_ABS))) {
            continue;
        }

        info = g_new0(MouseInfo, 1);
        info->index = s->id;
        info->name = g_strdup(s->handler->name);
        info->absolute = s->handler->mask & INPUT_EVENT_MASK_ABS;
        info->current = current;

        current = false;
        QAPI_LIST_PREPEND(mice_list, info);
    }

    return mice_list;
}

bool qemu_mouse_set(int index, Error **errp)
{
    QemuInputHandlerState *s;

    QTAILQ_FOREACH(s, &handlers, node) {
        if (s->id == index) {
            break;
        }
    }

    if (!s) {
        error_setg(errp, "Mouse at index '%d' not found", index);
        return false;
    }

    if (!(s->handler->mask & (INPUT_EVENT_MASK_REL |
                              INPUT_EVENT_MASK_ABS))) {
        error_setg(errp, "Input device '%s' is not a mouse",
                   s->handler->name);
        return false;
    }

    qemu_input_handler_activate(s);
    return true;
}

void qemu_input_touch_event(QemuConsole *con,
                            struct touch_slot touch_slots[INPUT_EVENT_SLOTS_MAX],
                            uint64_t num_slot,
                            int width, int height,
                            double x, double y,
                            InputMultiTouchType type,
                            Error **errp)
{
    struct touch_slot *slot;
    bool needs_sync = false;
    int update;
    int i;

    if (num_slot >= INPUT_EVENT_SLOTS_MAX) {
        error_setg(errp,
                   "Unexpected touch slot number: % " PRId64" >= %d",
                   num_slot, INPUT_EVENT_SLOTS_MAX);
        return;
    }

    slot = &touch_slots[num_slot];
    slot->x = x;
    slot->y = y;

    if (type == INPUT_MULTI_TOUCH_TYPE_BEGIN) {
        slot->tracking_id = num_slot;
    }

    for (i = 0; i < INPUT_EVENT_SLOTS_MAX; ++i) {
        if (i == num_slot) {
            update = type;
        } else {
            update = INPUT_MULTI_TOUCH_TYPE_UPDATE;
        }

        slot = &touch_slots[i];

        if (slot->tracking_id == -1) {
            continue;
        }

        if (update == INPUT_MULTI_TOUCH_TYPE_END) {
            slot->tracking_id = -1;
            qemu_input_queue_mtt(con, update, i, slot->tracking_id);
            needs_sync = true;
        } else {
            qemu_input_queue_mtt(con, update, i, slot->tracking_id);
            qemu_input_queue_btn(con, INPUT_BUTTON_TOUCH, true);
            qemu_input_queue_mtt_abs(con,
                                    INPUT_AXIS_X, (int) slot->x,
                                    0, width,
                                    i, slot->tracking_id);
            qemu_input_queue_mtt_abs(con,
                                    INPUT_AXIS_Y, (int) slot->y,
                                    0, height,
                                    i, slot->tracking_id);
            needs_sync = true;
        }
    }

    if (needs_sync) {
        qemu_input_event_sync();
    }
}

void qmp_send_key(KeyValueList *keys, bool has_hold_time, int64_t hold_time,
                  Error **errp)
{
    KeyValueList *p;
    unsigned int *up = NULL;
    int count = 0;

    if (!has_hold_time) {
        hold_time = 0; /* use default */
    }

    for (p = keys; p != NULL; p = p->next) {
        up = g_realloc_n(up, count + 1, sizeof(*up));
        up[count] = qemu_input_key_value_to_linux(p->value);
        qemu_input_event_send_key_linux(NULL, up[count], true);
        qemu_input_event_send_key_delay(hold_time);
        count++;
    }
    while (count) {
        count--;
        qemu_input_event_send_key_linux(NULL, up[count], false);
        qemu_input_event_send_key_delay(hold_time);
    }
    g_free(up);
}
