/*
 * MCP monitor: the Model Context Protocol, for AI agents
 *
 * The third monitor type, next to HMP and QMP.  It speaks JSON-RPC 2.0,
 * one message per line, as MCP's stdio transport does.  Tools are QAPI
 * commands with the feature 'mcp' (see qapi/mcp.json and the generated
 * qapi-mcp-tools.c), plus the hand-written 'hmp' and 'qmp' tools.
 *
 * Each tools/call runs in its own coroutine, so a call that waits for
 * the screen does not block the guest or other calls.  Cancelling a
 * call, or disconnecting, stops its input and releases every key and
 * button it pressed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/aio.h"
#include "monitor-internal.h"
#include "monitor/mcp.h"
#include "monitor/mcp-common.h"
#include "monitor/mcp-tools.h"
#include "qapi/error.h"
#include "qapi/qapi-emit-events.h"
#include "qapi/qmp-registry.h"
#include "qemu/coroutine.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qobject/qnull.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"
#include "qom/object_interfaces.h"
#include "trace.h"

#define MCP_EVENTS_MAX          32
#define MCP_PROGRESS_MS         5000

static const char mcp_tool_qmp[] =
    "{\"name\":\"qmp\","
    "\"description\":\"Run any QMP command and return its JSON result.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"execute\":{\"type\":\"string\",\"description\":\"the QMP command\"},"
    "\"arguments\":{\"type\":\"object\","
    "\"description\":\"its arguments\"}},"
    "\"required\":[\"execute\"]},"
    "\"annotations\":{\"destructiveHint\":true,\"idempotentHint\":false,"
    "\"openWorldHint\":false}}";

#ifdef CONFIG_HMP
static const char mcp_tool_hmp[] =
    "{\"name\":\"hmp\","
    "\"description\":\"Run one HMP (human monitor) command line and return"
    " its text output, for example 'info registers'.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\","
    "\"description\":\"the command line\"}},"
    "\"required\":[\"command\"]},"
    "\"annotations\":{\"destructiveHint\":true,\"idempotentHint\":false,"
    "\"openWorldHint\":false}}";
#endif

typedef struct McpCall {
    MonitorMCP *mon;
    QObject *id;
    char *tool;
    QDict *args;
    QObject *progress_token;
    QEMUTimer *progress_timer;
    int64_t start_ms;
    bool discard;               /* cancelled or client gone: no reply */
    QemuInputCtx ctx;
    QTAILQ_ENTRY(McpCall) node;
} McpCall;

typedef struct McpEvent {
    char *name;
    int64_t ms;                 /* the newest one, if repeated */
    int count;
} McpEvent;

struct MonitorMCP {
    Monitor parent_obj;
    JSONMessageParser parser;
    bool initialized;
    QTAILQ_HEAD(, McpCall) calls;
    McpEvent events[MCP_EVENTS_MAX];
    int n_events;
};

struct MonitorMCPClass {
    MonitorClass parent_class;
};

OBJECT_DEFINE_TYPE(MonitorMCP, monitor_mcp, MONITOR_MCP, MONITOR);

/* The call running in each coroutine, for mcp_current_input_ctx() */
static GHashTable *mcp_calls_by_co;

static void mcp_handle_message(void *opaque, QObject *msg, Error *err);

QemuInputCtx *mcp_current_input_ctx(void)
{
    McpCall *call;

    if (!mcp_calls_by_co || !qemu_in_coroutine()) {
        return NULL;
    }
    call = g_hash_table_lookup(mcp_calls_by_co, qemu_coroutine_self());
    return call ? &call->ctx : NULL;
}

static void mcp_send(MonitorMCP *mcp, QDict *msg)
{
    Monitor *mon = MONITOR(mcp);
    GString *json = qobject_to_json(QOBJECT(msg));

    trace_mcp_send(mcp, json->len);
    WITH_QEMU_LOCK_GUARD(&mon->mon_lock) {
        g_string_append_len(mon->outbuf, json->str, json->len);
        g_string_append_c(mon->outbuf, '\n');
        monitor_flush_locked(mon);
    }
    g_string_free(json, true);
}

static QDict *mcp_message(QObject *id)
{
    QDict *msg = qdict_new();

    qdict_put_str(msg, "jsonrpc", "2.0");
    if (id) {
        qdict_put_obj(msg, "id", qobject_ref(id));
    }
    return msg;
}

/* Send @result (taking the reference) as the reply to request @id */
static void mcp_reply(MonitorMCP *mcp, QObject *id, QObject *result)
{
    QDict *msg = mcp_message(id);

    qdict_put_obj(msg, "result", result);
    mcp_send(mcp, msg);
    qobject_unref(msg);
}

static void mcp_reply_error(MonitorMCP *mcp, QObject *id, int code,
                            const char *text, QObject *data)
{
    QDict *msg = mcp_message(NULL);
    QDict *error = qdict_new();

    qdict_put_obj(msg, "id", id ? qobject_ref(id) : QOBJECT(qnull()));
    qdict_put_int(error, "code", code);
    qdict_put_str(error, "message", text);
    if (data) {
        qdict_put_obj(error, "data", data);
    }
    qdict_put(msg, "error", error);
    mcp_send(mcp, msg);
    qobject_unref(msg);
}

/* Events */

static void monitor_mcp_emit_event(Monitor *mon, QAPIEvent event,
                                   QDict *qdict)
{
    MonitorMCP *mcp = MONITOR_MCP(mon);
    const char *name = QAPIEvent_str(event);

    /* a guest that sets its clock every second sends a stream of these */
    if (mcp->n_events &&
        !strcmp(mcp->events[mcp->n_events - 1].name, name)) {
        mcp->events[mcp->n_events - 1].count++;
        mcp->events[mcp->n_events - 1].ms =
            qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        return;
    }
    if (mcp->n_events == MCP_EVENTS_MAX) {
        g_free(mcp->events[0].name);
        memmove(&mcp->events[0], &mcp->events[1],
                sizeof(mcp->events[0]) * (MCP_EVENTS_MAX - 1));
        mcp->n_events--;
    }
    mcp->events[mcp->n_events].name = g_strdup(name);
    mcp->events[mcp->n_events].ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    mcp->events[mcp->n_events].count = 1;
    mcp->n_events++;
}

/* The events since the previous call, as a list; NULL if none */
static QList *mcp_take_events(MonitorMCP *mcp)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    QList *list;
    int i;

    if (!mcp->n_events) {
        return NULL;
    }
    list = qlist_new();
    for (i = 0; i < mcp->n_events; i++) {
        QDict *e = qdict_new();

        qdict_put_str(e, "event", mcp->events[i].name);
        qdict_put_int(e, "ms-ago", now - mcp->events[i].ms);
        if (mcp->events[i].count > 1) {
            qdict_put_int(e, "count", mcp->events[i].count);
        }
        qlist_append(list, e);
        g_free(mcp->events[i].name);
    }
    mcp->n_events = 0;
    return list;
}

/* Tool results */

static QDict *mcp_text_block(const char *text)
{
    QDict *b = qdict_new();

    qdict_put_str(b, "type", "text");
    qdict_put_str(b, "text", text);
    return b;
}

static QDict *mcp_tool_result(QList *content, bool is_error)
{
    QDict *r = qdict_new();

    qdict_put(r, "content", content);
    qdict_put_bool(r, "isError", is_error);
    return r;
}

static QDict *mcp_tool_error(const char *text)
{
    QList *content = qlist_new();

    qlist_append(content, mcp_text_block(text));
    return mcp_tool_result(content, true);
}

/*
 * Turn an McpResult-based return value into MCP content: the summary
 * as text, each image as an image block, all else as one JSON block.
 */
static QDict *mcp_content_from_result(MonitorMCP *mcp, QObject *ret)
{
    QDict *dict = qobject_to(QDict, ret);
    QList *content = qlist_new();
    g_autoptr(QDict) rest = qdict_new();
    QList *events = mcp_take_events(mcp);
    const QDictEntry *e;

    if (!dict) {
        GString *json = qobject_to_json(ret);

        qlist_append(content, mcp_text_block(json->str));
        g_string_free(json, true);
        if (events) {
            qdict_put(rest, "events", events);
            json = qobject_to_json(QOBJECT(rest));
            qlist_append(content, mcp_text_block(json->str));
            g_string_free(json, true);
        }
        return mcp_tool_result(content, false);
    }

    if (qdict_haskey(dict, "summary")) {
        qlist_append(content,
                     mcp_text_block(qdict_get_str(dict, "summary")));
    }
    for (e = qdict_first(dict); e; e = qdict_next(dict, e)) {
        const char *key = qdict_entry_key(e);
        QList *images, *meta;
        QListEntry *le;

        if (!strcmp(key, "summary")) {
            continue;
        }
        if (strcmp(key, "images")) {
            qdict_put_obj(rest, key, qobject_ref(qdict_entry_value(e)));
            continue;
        }
        images = qobject_to(QList, qdict_entry_value(e));
        meta = qlist_new();
        QLIST_FOREACH_ENTRY(images, le) {
            QDict *img = qobject_to(QDict, qlist_entry_obj(le));
            QDict *block = qdict_new();
            QDict *m = qdict_clone_shallow(img);

            qdict_put_str(block, "type", "image");
            qdict_put_str(block, "data", qdict_get_str(img, "data"));
            qdict_put_str(block, "mimeType",
                          qdict_get_str(img, "mime-type"));
            qlist_append(content, block);
            qdict_del(m, "data");
            qdict_del(m, "mime-type");
            qlist_append(meta, m);
        }
        qdict_put(rest, "images", meta);
    }
    if (events) {
        qdict_put(rest, "events", events);
    }
    if (qdict_size(rest)) {
        GString *json = qobject_to_json(QOBJECT(rest));

        qlist_append(content, mcp_text_block(json->str));
        g_string_free(json, true);
    }
    return mcp_tool_result(content, false);
}

/* Run QMP command @execute with @args from the call's coroutine */
static QDict *coroutine_fn mcp_dispatch(McpCall *call, const char *execute,
                                        QDict *args)
{
    QDict *req = qdict_new();
    QDict *rsp;

    qdict_put_str(req, "execute", execute);
    qdict_put(req, "arguments", qobject_ref(args));
    rsp = qmp_dispatch(&qmp_commands, QOBJECT(req), false,
                       MONITOR(call->mon));
    qobject_unref(req);
    return rsp;
}

static QDict *mcp_error_from_rsp(QDict *rsp)
{
    QDict *error = qdict_get_qdict(rsp, "error");

    return mcp_tool_error(error ? qdict_get_try_str(error, "desc")
                                : "unknown error");
}

static void mcp_progress_tick(void *opaque)
{
    McpCall *call = opaque;
    QDict *msg = mcp_message(NULL);
    QDict *params = qdict_new();
    int64_t s = (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - call->start_ms)
                / 1000;
    g_autofree char *text = g_strdup_printf("%s running for %" PRId64 " s",
                                            call->tool, s);

    if (call->discard) {
        qobject_unref(msg);
        qobject_unref(params);
        return;
    }
    qdict_put_str(msg, "method", "notifications/progress");
    qdict_put_obj(params, "progressToken",
                  qobject_ref(call->progress_token));
    qdict_put_int(params, "progress", s);
    qdict_put_str(params, "message", text);
    qdict_put(msg, "params", params);
    mcp_send(call->mon, msg);
    qobject_unref(msg);
    timer_mod(call->progress_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + MCP_PROGRESS_MS);
}

static void mcp_call_free(McpCall *call)
{
    qemu_input_ctx_end(&call->ctx);
    QTAILQ_REMOVE(&call->mon->calls, call, node);
    if (call->progress_timer) {
        timer_free(call->progress_timer);
    }
    qobject_unref(call->id);
    qobject_unref(call->args);
    qobject_unref(call->progress_token);
    g_free(call->tool);
    object_unref(OBJECT(call->mon));
    g_free(call);
}

static void coroutine_fn mcp_call_co(void *opaque)
{
    McpCall *call = opaque;
    MonitorMCP *mcp = call->mon;
    QDict *result = NULL;
    QDict *rsp = NULL;

    g_hash_table_insert(mcp_calls_by_co, qemu_coroutine_self(), call);
    trace_mcp_tool_start(mcp, call->tool);

    if (!strcmp(call->tool, "qmp")) {
        const char *execute = qdict_get_try_str(call->args, "execute");
        QDict *args = qdict_get_qdict(call->args, "arguments");

        if (!execute) {
            result = mcp_tool_error("'execute' is missing");
        } else if (!strcmp(execute, "qmp_capabilities")) {
            result = mcp_tool_error("qmp_capabilities is not needed here");
        } else {
            g_autoptr(QDict) empty = qdict_new();

            rsp = mcp_dispatch(call, execute, args ? args : empty);
        }
#ifdef CONFIG_HMP
    } else if (!strcmp(call->tool, "hmp")) {
        const char *cmd = qdict_get_try_str(call->args, "command");
        g_autoptr(QDict) args = qdict_new();

        if (!cmd) {
            result = mcp_tool_error("'command' is missing");
        } else {
            qdict_put_str(args, "command-line", cmd);
            rsp = mcp_dispatch(call, "human-monitor-command", args);
        }
#endif
    } else {
        rsp = mcp_dispatch(call, call->tool, call->args);
    }

    if (rsp && qdict_haskey(rsp, "error")) {
        result = mcp_error_from_rsp(rsp);
    } else if (rsp) {
        QObject *ret = qdict_get(rsp, "return");
        QString *text = qobject_to(QString, ret);

        if (text) {
            /* hmp: plain text */
            QList *content = qlist_new();
            QList *events = mcp_take_events(mcp);
            g_autoptr(GString) s = g_string_new(NULL);
            const char *p;

            /* the monitor writes \r\n line ends */
            for (p = qstring_get_str(text); *p; p++) {
                if (p[0] != '\r' || p[1] != '\n') {
                    g_string_append_c(s, *p);
                }
            }
            qlist_append(content,
                         mcp_text_block(s->len ? s->str : "(no output)"));
            if (events) {
                g_autoptr(QDict) rest = qdict_new();
                GString *json;

                qdict_put(rest, "events", events);
                json = qobject_to_json(QOBJECT(rest));
                qlist_append(content, mcp_text_block(json->str));
                g_string_free(json, true);
            }
            result = mcp_tool_result(content, false);
        } else {
            result = mcp_content_from_result(mcp, ret);
        }
    }
    qobject_unref(rsp);

    g_hash_table_remove(mcp_calls_by_co, qemu_coroutine_self());
    trace_mcp_tool_done(mcp, call->tool, call->discard,
                        qdict_get_bool(result, "isError"));
    if (!call->discard) {
        mcp_reply(mcp, call->id, QOBJECT(result));
    } else {
        qobject_unref(result);
    }
    mcp_call_free(call);
}

/* Requests */

static bool mcp_tool_exists(const char *name)
{
    const McpToolDef *t;
    const QmpCommand *cmd;

    if (!strcmp(name, "qmp")) {
        return true;
    }
#ifdef CONFIG_HMP
    if (!strcmp(name, "hmp")) {
        return true;
    }
#endif
    for (t = mcp_tools; t->name; t++) {
        if (!strcmp(t->name, name)) {
            cmd = qmp_find_command(&qmp_commands, name);
            return cmd && cmd->enabled;
        }
    }
    return false;
}

static QList *mcp_tools_list(void)
{
    QList *list = qlist_new();
    const McpToolDef *t;

    for (t = mcp_tools; t->name; t++) {
        if (mcp_tool_exists(t->name)) {
            qlist_append_obj(list, qobject_from_json(t->json, &error_abort));
        }
    }
#ifdef CONFIG_HMP
    qlist_append_obj(list, qobject_from_json(mcp_tool_hmp, &error_abort));
#endif
    qlist_append_obj(list, qobject_from_json(mcp_tool_qmp, &error_abort));
    return list;
}

static void mcp_initialize(MonitorMCP *mcp, QObject *id, QDict *params)
{
    const char *asked = params ? qdict_get_try_str(params, "protocolVersion")
                               : NULL;
    QDict *result = qdict_new();
    QDict *caps = qdict_new();
    QDict *tools = qdict_new();
    QDict *info = qdict_new();

    qdict_put_str(result, "protocolVersion",
                  mcp_protocol_supported(asked) ? asked
                                                : MCP_PROTOCOL_LATEST);
    qdict_put_bool(tools, "listChanged", false);
    qdict_put(caps, "tools", tools);
    qdict_put(result, "capabilities", caps);
    qdict_put_str(info, "name", "lemu");
    qdict_put_str(info, "version", QEMU_VERSION "-mcp");
    qdict_put(result, "serverInfo", info);
    qdict_put_str(result, "instructions", mcp_instructions);
    mcp->initialized = true;
    mcp_reply(mcp, id, QOBJECT(result));
}

static void mcp_tools_call(MonitorMCP *mcp, QObject *id, QDict *params)
{
    const char *name = params ? qdict_get_try_str(params, "name") : NULL;
    QDict *args = params ? qdict_get_qdict(params, "arguments") : NULL;
    QDict *meta = params ? qdict_get_qdict(params, "_meta") : NULL;
    QObject *token = meta ? qdict_get(meta, "progressToken") : NULL;
    McpCall *call;
    Coroutine *co;

    if (!name || !mcp_tool_exists(name)) {
        g_autofree char *text = g_strdup_printf("unknown tool '%s'",
                                                name ? name : "");
        mcp_reply_error(mcp, id, MCP_INVALID_PARAMS, text, NULL);
        return;
    }

    call = g_new0(McpCall, 1);
    call->mon = mcp;
    object_ref(OBJECT(mcp));
    call->id = qobject_ref(id);
    call->tool = g_strdup(name);
    call->args = args ? qobject_ref(args) : qdict_new();
    call->start_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    qemu_input_ctx_init(&call->ctx, mcp);
    if (token) {
        call->progress_token = qobject_ref(token);
        call->progress_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                            mcp_progress_tick, call);
        timer_mod(call->progress_timer, call->start_ms + MCP_PROGRESS_MS);
    }
    QTAILQ_INSERT_TAIL(&mcp->calls, call, node);

    co = qemu_coroutine_create(mcp_call_co, call);
    aio_co_enter(iohandler_get_aio_context(), co);
}

static void mcp_cancel_call(McpCall *call)
{
    call->discard = true;
    qemu_input_ctx_cancel(&call->ctx);
}

static void mcp_notification(MonitorMCP *mcp, const char *method,
                             QDict *params)
{
    if (!strcmp(method, "notifications/cancelled") && params) {
        QObject *id = qdict_get(params, "requestId");
        McpCall *call;

        QTAILQ_FOREACH(call, &mcp->calls, node) {
            if (id && qobject_is_equal(id, call->id)) {
                mcp_cancel_call(call);
            }
        }
    }
    /* notifications/initialized and others need no action */
}

static void mcp_handle_message(void *opaque, QObject *msg, Error *err)
{
    MonitorMCP *mcp = opaque;
    QDict *dict = qobject_to(QDict, msg);
    QDict *params;
    QObject *id;
    const char *method;
    const char *version;

    if (err) {
        mcp_reply_error(mcp, NULL, MCP_PARSE_ERROR,
                        error_get_pretty(err), NULL);
        error_free(err);
        return;
    }
    if (!dict) {
        mcp_reply_error(mcp, NULL, MCP_INVALID_REQUEST,
                        qobject_type(msg) == QTYPE_QLIST
                        ? "batches are not supported"
                        : "a message must be a JSON object", NULL);
        goto out;
    }

    method = qdict_get_try_str(dict, "method");
    id = qdict_get(dict, "id");
    params = qdict_get_qdict(dict, "params");
    trace_mcp_request(mcp, method ? method : "(response)");

    if (!method) {
        /* a response; lemu never sends requests */
        goto out;
    }
    if (!id) {
        mcp_notification(mcp, method, params);
        goto out;
    }

    version = mcp_meta_protocol(params);
    if (!mcp->initialized && version && !mcp_protocol_supported(version)) {
        QDict *data = qdict_new();

        qdict_put(data, "supported", mcp_protocol_list());
        qdict_put_str(data, "requested", version);
        mcp_reply_error(mcp, id, MCP_UNSUPPORTED_VERSION,
                        "Unsupported protocol version", QOBJECT(data));
        goto out;
    }

    if (!strcmp(method, "initialize")) {
        mcp_initialize(mcp, id, params);
    } else if (!strcmp(method, "ping")) {
        mcp_reply(mcp, id, QOBJECT(qdict_new()));
    } else if (!strcmp(method, "tools/list")) {
        QDict *result = qdict_new();

        qdict_put(result, "tools", mcp_tools_list());
        mcp_reply(mcp, id, QOBJECT(result));
    } else if (!strcmp(method, "tools/call")) {
        mcp_tools_call(mcp, id, params);
    } else {
        g_autofree char *text = g_strdup_printf("unknown method '%s'",
                                                method);
        mcp_reply_error(mcp, id, MCP_METHOD_NOT_FOUND, text, NULL);
    }
out:
    qobject_unref(msg);
}

/* Monitor plumbing */

static void monitor_mcp_read(void *opaque, const uint8_t *buf, int size)
{
    MonitorMCP *mcp = opaque;

    json_message_parser_feed(&mcp->parser, (const char *)buf, size);
}

static void monitor_mcp_event(void *opaque, QEMUChrEvent event)
{
    MonitorMCP *mcp = opaque;
    McpCall *call;
    int i;

    switch (event) {
    case CHR_EVENT_OPENED:
    case CHR_EVENT_CLOSED:
        /* a new client starts a new session */
        QTAILQ_FOREACH(call, &mcp->calls, node) {
            mcp_cancel_call(call);
        }
        qemu_input_release_owner(mcp);
        json_message_parser_destroy(&mcp->parser);
        json_message_parser_init(&mcp->parser, mcp_handle_message, mcp,
                                 NULL);
        mcp->initialized = false;
        for (i = 0; i < mcp->n_events; i++) {
            g_free(mcp->events[i].name);
        }
        mcp->n_events = 0;
        if (event == CHR_EVENT_CLOSED) {
            WITH_QEMU_LOCK_GUARD(&mcp->parent_obj.mon_lock) {
                g_string_truncate(mcp->parent_obj.outbuf, 0);
            }
        }
        break;
    default:
        break;
    }
}

static bool monitor_mcp_requires_iothread(const Monitor *mon)
{
    return false;
}

static void monitor_mcp_accept_input(Monitor *mon)
{
    WITH_QEMU_LOCK_GUARD(&mon->mon_lock) {
        qemu_chr_fe_accept_input(&mon->chr);
    }
}

static void monitor_mcp_complete(UserCreatable *uc, Error **errp)
{
    MonitorMCP *mcp = MONITOR_MCP(uc);
    UserCreatableClass *ucc_parent =
        USER_CREATABLE_CLASS(
            object_class_get_parent(
                OBJECT_CLASS(MONITOR_MCP_GET_CLASS(mcp))));
    ERRP_GUARD();

    ucc_parent->complete(uc, errp);
    if (*errp) {
        return;
    }
    qemu_chr_fe_set_handlers(&mcp->parent_obj.chr, monitor_can_read,
                             monitor_mcp_read, monitor_mcp_event,
                             NULL, mcp, NULL, true);
    monitor_list_append(&mcp->parent_obj);
}

static bool monitor_mcp_prepare_delete(UserCreatable *uc, Error **errp)
{
    Monitor *mon = MONITOR(uc);
    MonitorMCP *mcp = MONITOR_MCP(uc);
    McpCall *call;

    if (!QTAILQ_EMPTY(&mcp->calls)) {
        QTAILQ_FOREACH(call, &mcp->calls, node) {
            mcp_cancel_call(call);
        }
        error_setg(errp, "the MCP monitor still has calls in flight");
        return false;
    }
    WITH_QEMU_LOCK_GUARD(&monitor_lock) {
        QTAILQ_REMOVE(&mon_list, mon, entry);
    }
    WITH_QEMU_LOCK_GUARD(&mon->mon_lock) {
        monitor_cancel_out_watch(mon);
    }
    qemu_chr_fe_set_handlers(&mon->chr, NULL, NULL, NULL, NULL,
                             NULL, NULL, true);
    qemu_input_release_owner(mcp);
    return true;
}

static void monitor_mcp_init(Object *obj)
{
    MonitorMCP *mcp = MONITOR_MCP(obj);

    QTAILQ_INIT(&mcp->calls);
    json_message_parser_init(&mcp->parser, mcp_handle_message, mcp, NULL);
    if (!mcp_calls_by_co) {
        mcp_calls_by_co = g_hash_table_new(NULL, NULL);
    }
}

static void monitor_mcp_finalize(Object *obj)
{
    MonitorMCP *mcp = MONITOR_MCP(obj);
    int i;

    json_message_parser_destroy(&mcp->parser);
    for (i = 0; i < mcp->n_events; i++) {
        g_free(mcp->events[i].name);
    }
}

static void monitor_mcp_class_init(ObjectClass *cls, const void *data)
{
    MonitorClass *moncls = MONITOR_CLASS(cls);
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(cls);

    moncls->emit_event = monitor_mcp_emit_event;
    moncls->requires_iothread = monitor_mcp_requires_iothread;
    moncls->accept_input = monitor_mcp_accept_input;
    ucc->complete = monitor_mcp_complete;
    ucc->prepare_delete = monitor_mcp_prepare_delete;
}

void monitor_new_mcp(const char *id, const char *chardev_id, Error **errp)
{
    g_autofree char *autoid = id ? NULL : monitor_compat_id();

    object_new_with_props(TYPE_MONITOR_MCP, object_get_objects_root(),
                          id ? id : autoid, errp,
                          "chardev", chardev_id, NULL);
}
