/*
 * lemu-mcp: the one MCP server an agent starts for all lemu VMs
 *
 * It speaks MCP on stdin and stdout, finds the running VMs through
 * the registry in /tmp/lemu-$UID, and forwards each tool call to the
 * VM named in its "vm" argument.  It answers initialize and tools/list
 * at once, and it never exits because a VM went away.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib-unix.h>
#include "monitor/mcp-common.h"
#include "monitor/mcp-tools.h"
#include "qapi/error.h"
#include "qapi/qapi-visit-sockets.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/sockets.h"
#include "qobject/qbool.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qobject/qnull.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"

#define LINE_MAX_BYTES      (64 * 1024 * 1024)
#define TOOLS_TIMEOUT_MS    3000

static const char bridge_instructions[] =
    "- Several VMs may be running, the user's and yours. Every tool takes"
    " vm: a VM name or PID from vm-list. You may leave it out only when"
    " exactly one VM is running and you started it.\n";

static const char vm_list_tool[] =
    "{\"name\":\"vm-list\","
    "\"description\":\"List the running lemu VMs: name, PID, folder,"
    " machine, start time, whether you (the agent) started it, and"
    " whether it answers. Pass a name or PID as vm to the other"
    " tools.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}},"
    "\"annotations\":{\"readOnlyHint\":true,\"openWorldHint\":false}}";

static const char vm_arg_schema[] =
    "{\"type\":\"string\","
    "\"description\":\"the VM: a name or PID from vm-list (needed when"
    " more than one VM is running)\"}";

typedef enum CallKind {
    CALL_CLIENT,                /* a tools/call from the client */
    CALL_INIT,                  /* our initialize */
    CALL_TOOLS,                 /* our tools/list */
} CallKind;

typedef struct Call {
    CallKind kind;
    int64_t vm_id;
    QObject *client_id;         /* CALL_CLIENT: the client's request id */
    char *line;                 /* the request, until it is sent */
} Call;

typedef struct Vm {
    int pid;
    char *name;
    char *endpoint;             /* "unix:PATH" or "tcp:HOST:PORT" */
    char *cwd;
    char *machine;
    char *started;
    char *session;
    bool by_agent;
    bool seen;                  /* in the latest registry scan */

    int fd;                     /* -1 when not connected */
    guint watch;
    GString *in;
    bool ready;                 /* our initialize was answered */
    int64_t next_id;
    GHashTable *calls;          /* vm_id -> Call, sent or waiting */
    GQueue waiting;             /* vm_ids to send once ready */
} Vm;

static GHashTable *vms;         /* pid -> Vm */
static char *registry_dir;
static const char *my_session;
static GString *client_in;
static GMainLoop *loop;
static bool client_initialized;
static QList *tools_cache;

/* A tools/list waiting for the first VM's answer */
static QObject *tools_client_id;
static guint tools_timer;

static bool write_all(int fd, const char *buf, size_t len)
{
    while (len) {
        ssize_t n = write(fd, buf, len);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        buf += n;
        len -= n;
    }
    return true;
}

static char *message_line(QDict *msg)
{
    GString *json = qobject_to_json(QOBJECT(msg));

    g_string_append_c(json, '\n');
    return g_string_free(json, false);
}

/* The client */

static void client_send(QDict *msg)
{
    g_autofree char *line = message_line(msg);

    if (!write_all(STDOUT_FILENO, line, strlen(line))) {
        /* the client is gone; nothing left to do */
        g_main_loop_quit(loop);
    }
}

static QDict *client_message(QObject *id)
{
    QDict *msg = qdict_new();

    qdict_put_str(msg, "jsonrpc", "2.0");
    qdict_put_obj(msg, "id", id ? qobject_ref(id) : QOBJECT(qnull()));
    return msg;
}

static void client_reply(QObject *id, QObject *result)
{
    g_autoptr(QDict) msg = client_message(id);

    qdict_put_obj(msg, "result", result);
    client_send(msg);
}

static void client_reply_error(QObject *id, int code, const char *text,
                               QObject *data)
{
    g_autoptr(QDict) msg = client_message(id);
    QDict *err = qdict_new();

    qdict_put_int(err, "code", code);
    qdict_put_str(err, "message", text);
    if (data) {
        qdict_put_obj(err, "data", data);
    }
    qdict_put(msg, "error", err);
    client_send(msg);
}

static QDict *text_block(const char *text)
{
    QDict *block = qdict_new();

    qdict_put_str(block, "type", "text");
    qdict_put_str(block, "text", text);
    return block;
}

static void client_tool_result(QObject *id, const char *text, QObject *json,
                               bool is_error)
{
    QDict *result = qdict_new();
    QList *content = qlist_new();

    qlist_append(content, text_block(text));
    if (json) {
        GString *s = qobject_to_json(json);

        qlist_append(content, text_block(s->str));
        g_string_free(s, true);
        qobject_unref(json);
    }
    qdict_put(result, "content", content);
    qdict_put_bool(result, "isError", is_error);
    client_reply(id, QOBJECT(result));
}

/* The registry */

static void call_free(gpointer p)
{
    Call *call = p;

    qobject_unref(call->client_id);
    g_free(call->line);
    g_free(call);
}

static Vm *vm_new(int pid)
{
    Vm *vm = g_new0(Vm, 1);

    vm->pid = pid;
    vm->fd = -1;
    vm->in = g_string_new(NULL);
    vm->next_id = 1;
    vm->calls = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL,
                                      call_free);
    g_queue_init(&vm->waiting);
    return vm;
}

static void tools_answer(QList *list);

static void vm_disconnect(Vm *vm, const char *why)
{
    GHashTableIter iter;
    Call *call;

    if (vm->fd >= 0) {
        if (vm->watch) {
            g_source_remove(vm->watch);
            vm->watch = 0;
        }
        close(vm->fd);
        vm->fd = -1;
        g_string_truncate(vm->in, 0);
        if (why) {
            error_report("VM %s (pid %d): %s", vm->name, vm->pid, why);
        }
    }
    vm->ready = false;

    g_hash_table_iter_init(&iter, vm->calls);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&call)) {
        if (call->kind == CALL_CLIENT) {
            g_autofree char *text = g_strdup_printf(
                "lost the connection to VM %s (pid %d)%s%s; the call may or"
                " may not have run. Check with vm-list.", vm->name, vm->pid,
                why ? ": " : "", why ? why : "");

            client_tool_result(call->client_id, text, NULL, true);
        } else if (call->kind == CALL_TOOLS) {
            tools_answer(NULL);
        }
    }
    g_hash_table_remove_all(vm->calls);
    g_queue_clear(&vm->waiting);
}

static void vm_free(gpointer p)
{
    Vm *vm = p;

    vm_disconnect(vm, NULL);
    g_hash_table_destroy(vm->calls);
    g_string_free(vm->in, true);
    g_free(vm->name);
    g_free(vm->endpoint);
    g_free(vm->cwd);
    g_free(vm->machine);
    g_free(vm->started);
    g_free(vm->session);
    g_free(vm);
}

static void set_str(char **field, const char *value)
{
    g_free(*field);
    *field = g_strdup(value ? value : "");
}

/* Where to reach the VM: its default socket, else an explicit -mcp */
static char *registry_endpoint(QDict *d)
{
    const char *sock = qdict_get_try_str(d, "socket");
    QDict *endpoints = qdict_get_qdict(d, "endpoints");
    QList *mcp = endpoints ? qdict_get_qlist(endpoints, "mcp") : NULL;
    const QListEntry *e;

    if (sock) {
        return g_strdup_printf("unix:%s", sock);
    }
    QLIST_FOREACH_ENTRY(mcp, e) {
        QString *s = qobject_to(QString, qlist_entry_obj(e));
        const char *spec = s ? qstring_get_str(s) : "";

        if (g_str_has_prefix(spec, "unix:") || g_str_has_prefix(spec, "tcp:")) {
            return g_strndup(spec, strcspn(spec, ","));
        }
    }
    return NULL;
}

static bool registry_dir_ok(void)
{
    struct stat st;

    if (lstat(registry_dir, &st) < 0) {
        return false;           /* no VM has run yet */
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 077)) {
        error_report("%s is not a private directory of this user; ignoring"
                     " it", registry_dir);
        return false;
    }
    return true;
}

static void registry_read_one(int pid, const char *path)
{
    g_autofree char *text = NULL;
    g_autoptr(QDict) d = NULL;
    Vm *vm;
    char *endpoint;

    if (kill(pid, 0) < 0 && errno == ESRCH) {
        return;                 /* stale; QEMU removes these */
    }
    if (!g_file_get_contents(path, &text, NULL, NULL)) {
        return;
    }
    d = qobject_to(QDict, qobject_from_json(text, NULL));
    if (!d) {
        return;
    }
    endpoint = registry_endpoint(d);
    if (!endpoint) {
        return;
    }

    vm = g_hash_table_lookup(vms, GINT_TO_POINTER(pid));
    if (!vm) {
        vm = vm_new(pid);
        g_hash_table_insert(vms, GINT_TO_POINTER(pid), vm);
    }
    if (vm->endpoint && strcmp(vm->endpoint, endpoint)) {
        vm_disconnect(vm, "its endpoint changed");
    }
    g_free(vm->endpoint);
    vm->endpoint = endpoint;
    set_str(&vm->name, qdict_get_try_str(d, "name"));
    set_str(&vm->cwd, qdict_get_try_str(d, "cwd"));
    set_str(&vm->machine, qdict_get_try_str(d, "machine"));
    set_str(&vm->started, qdict_get_try_str(d, "started"));
    set_str(&vm->session, qdict_get_try_str(d, "session"));
    vm->by_agent = qdict_get_try_bool(d, "by-agent", false);
    vm->seen = true;
}

static gint vm_compare(gconstpointer a, gconstpointer b)
{
    const Vm *x = *(Vm *const *)a, *y = *(Vm *const *)b;
    int c = strcmp(x->started, y->started);

    return c ? c : x->pid - y->pid;
}

/* Read the registry; returns the VMs, oldest first */
static GPtrArray *registry_scan(void)
{
    GPtrArray *list = g_ptr_array_new();
    GHashTableIter iter;
    GDir *dir;
    const char *file;
    Vm *vm;

    g_hash_table_iter_init(&iter, vms);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&vm)) {
        vm->seen = false;
    }

    dir = registry_dir_ok() ? g_dir_open(registry_dir, 0, NULL) : NULL;
    while (dir && (file = g_dir_read_name(dir))) {
        g_autofree char *path = NULL;
        const char *end;
        int pid;

        if (qemu_strtoi(file, &end, 10, &pid) < 0 || pid <= 0 ||
            strcmp(end, ".json")) {
            continue;
        }
        path = g_build_filename(registry_dir, file, NULL);
        registry_read_one(pid, path);
    }
    if (dir) {
        g_dir_close(dir);
    }

    g_hash_table_iter_init(&iter, vms);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&vm)) {
        if (!vm->seen) {
            g_hash_table_iter_remove(&iter);
        } else {
            g_ptr_array_add(list, vm);
        }
    }
    g_ptr_array_sort(list, vm_compare);
    return list;
}

/* VM connections */

static bool vm_write(Vm *vm, const char *line)
{
    if (!write_all(vm->fd, line, strlen(line))) {
        g_autofree char *why = g_strdup_printf("write failed: %s",
                                               strerror(errno));

        vm_disconnect(vm, why);
        return false;
    }
    return true;
}

/* Send @msg as a request; it waits until our initialize is answered */
static void vm_request(Vm *vm, CallKind kind, QObject *client_id,
                       QDict *msg)
{
    Call *call = g_new0(Call, 1);

    call->kind = kind;
    call->vm_id = vm->next_id++;
    call->client_id = client_id ? qobject_ref(client_id) : NULL;
    qdict_del(msg, "id");
    qdict_put_int(msg, "id", call->vm_id);
    call->line = message_line(msg);
    g_hash_table_insert(vm->calls, &call->vm_id, call);

    if (vm->ready || kind == CALL_INIT) {
        g_autofree char *line = g_steal_pointer(&call->line);

        vm_write(vm, line);
    } else {
        g_queue_push_tail(&vm->waiting, GINT_TO_POINTER(call->vm_id));
    }
}

static QDict *request(const char *method, QDict *params)
{
    QDict *msg = qdict_new();

    qdict_put_str(msg, "jsonrpc", "2.0");
    qdict_put_str(msg, "method", method);
    if (params) {
        qdict_put(msg, "params", params);
    }
    return msg;
}

static void vm_ready(Vm *vm)
{
    g_autoptr(QDict) note = request("notifications/initialized", NULL);
    g_autofree char *line = message_line(note);

    vm->ready = true;
    if (!vm_write(vm, line)) {
        return;
    }
    while (vm->fd >= 0 && !g_queue_is_empty(&vm->waiting)) {
        int64_t id = GPOINTER_TO_INT(g_queue_pop_head(&vm->waiting));
        Call *call = g_hash_table_lookup(vm->calls, &id);
        g_autofree char *send = call ? g_steal_pointer(&call->line) : NULL;

        if (send) {
            vm_write(vm, send);
        }
    }
}

static QList *tools_from_vm(QDict *result);

static void vm_response(Vm *vm, QDict *msg)
{
    QNum *num = qobject_to(QNum, qdict_get(msg, "id"));
    QDict *result = qdict_get_qdict(msg, "result");
    QDict *err = qdict_get_qdict(msg, "error");
    int64_t id;
    Call *call;

    if (!num || !qnum_get_try_int(num, &id)) {
        return;
    }
    call = g_hash_table_lookup(vm->calls, &id);
    if (!call) {
        return;                 /* cancelled, or not ours */
    }

    switch (call->kind) {
    case CALL_INIT:
        g_hash_table_remove(vm->calls, &id);
        if (result) {
            vm_ready(vm);
        } else {
            vm_disconnect(vm, err ? qdict_get_try_str(err, "message")
                                  : "bad initialize reply");
        }
        return;
    case CALL_TOOLS:
        tools_answer(result ? tools_from_vm(result) : NULL);
        break;
    case CALL_CLIENT:
        qdict_del(msg, "id");
        qdict_put_obj(msg, "id", qobject_ref(call->client_id));
        client_send(msg);
        break;
    }
    g_hash_table_remove(vm->calls, &id);
}

static void vm_message(Vm *vm, const char *line)
{
    QObject *obj = qobject_from_json(line, NULL);
    g_autoptr(QDict) msg = qobject_to(QDict, obj);

    if (!msg) {
        qobject_unref(obj);
        error_report("VM %s (pid %d) sent something that is not a JSON"
                     " object; ignored", vm->name, vm->pid);
        return;
    }
    if (!qdict_haskey(msg, "method")) {
        vm_response(vm, msg);
    } else if (!qdict_haskey(msg, "id")) {
        /* progress and other notifications go to the client as they are */
        client_send(msg);
    }
    /* lemu never sends requests */
}

static gboolean vm_readable(int fd, GIOCondition cond, gpointer opaque)
{
    Vm *vm = opaque;
    char buf[65536];
    ssize_t n;
    char *nl;

    n = read(fd, buf, sizeof(buf));
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
        return G_SOURCE_CONTINUE;
    }
    if (n <= 0) {
        vm->watch = 0;
        vm_disconnect(vm, n ? strerror(errno) : "connection closed");
        return G_SOURCE_REMOVE;
    }
    g_string_append_len(vm->in, buf, n);
    while (vm->fd >= 0 && (nl = memchr(vm->in->str, '\n', vm->in->len))) {
        g_autofree char *line = g_strndup(vm->in->str, nl - vm->in->str);

        g_string_erase(vm->in, 0, nl - vm->in->str + 1);
        vm_message(vm, line);
    }
    if (vm->in->len > LINE_MAX_BYTES) {
        vm_disconnect(vm, "message too long");
    }
    return G_SOURCE_CONTINUE;
}

static bool vm_connect(Vm *vm, Error **errp)
{
    g_autoptr(SocketAddress) addr = NULL;
    QDict *params, *caps, *info;
    int fd;

    if (vm->fd >= 0) {
        return true;
    }
    addr = socket_parse(vm->endpoint, errp);
    if (!addr) {
        return false;
    }
    fd = socket_connect(addr, errp);
    if (fd < 0) {
        return false;
    }
    vm->fd = fd;
    vm->watch = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR,
                              vm_readable, vm);

    params = qdict_new();
    caps = qdict_new();
    info = qdict_new();
    qdict_put_str(params, "protocolVersion", MCP_PROTOCOL_LATEST);
    qdict_put(params, "capabilities", caps);
    qdict_put_str(info, "name", "lemu-mcp");
    qdict_put_str(info, "version", QEMU_VERSION);
    qdict_put(params, "clientInfo", info);
    {
        g_autoptr(QDict) msg = request("initialize", params);

        vm_request(vm, CALL_INIT, NULL, msg);
    }
    return vm->fd >= 0;
}

/* tools/list */

static void tool_add_vm_arg(QDict *tool)
{
    QDict *schema = qdict_get_qdict(tool, "inputSchema");
    QDict *props = schema ? qdict_get_qdict(schema, "properties") : NULL;

    if (props && !qdict_haskey(props, "vm")) {
        qdict_put_obj(props, "vm", qobject_from_json(vm_arg_schema,
                                                     &error_abort));
    }
}

static QList *tools_finish(QList *list)
{
    const QListEntry *e;

    QLIST_FOREACH_ENTRY(list, e) {
        QDict *tool = qobject_to(QDict, qlist_entry_obj(e));

        if (tool) {
            tool_add_vm_arg(tool);
        }
    }
    qlist_append_obj(list, qobject_from_json(vm_list_tool, &error_abort));
    return list;
}

static QList *tools_from_vm(QDict *result)
{
    QList *tools = qdict_get_qlist(result, "tools");

    return tools ? tools_finish(qlist_copy(tools)) : NULL;
}

static QList *tools_builtin(void)
{
    QList *list = qlist_new();
    const McpToolDef *t;

    for (t = mcp_tools; t->name; t++) {
        qlist_append_obj(list, qobject_from_json(t->json, &error_abort));
    }
    return tools_finish(list);
}

static void tools_reply(QObject *id, QList *list)
{
    QDict *result = qdict_new();

    qdict_put(result, "tools", list);
    client_reply(id, QOBJECT(result));
}

/* The first VM answered tools/list (@list), or failed or timed out */
static void tools_answer(QList *list)
{
    if (list && !tools_cache) {
        tools_cache = qlist_copy(list);
    }
    if (tools_client_id) {
        tools_reply(tools_client_id, list ? qlist_copy(list)
                                          : tools_builtin());
        qobject_unref(tools_client_id);
        tools_client_id = NULL;
        if (tools_timer) {
            g_source_remove(tools_timer);
            tools_timer = 0;
        }
    }
    qobject_unref(list);
}

static gboolean tools_timeout(gpointer opaque)
{
    tools_timer = 0;
    tools_answer(NULL);
    return G_SOURCE_REMOVE;
}

static void client_tools_list(QObject *id)
{
    g_autoptr(GPtrArray) list = NULL;
    guint i;

    if (tools_cache) {
        tools_reply(id, qlist_copy(tools_cache));
        return;
    }
    if (tools_client_id) {
        tools_reply(id, tools_builtin());
        return;
    }
    list = registry_scan();
    for (i = 0; i < list->len; i++) {
        Vm *vm = g_ptr_array_index(list, i);

        if (vm_connect(vm, NULL)) {
            g_autoptr(QDict) msg = request("tools/list", NULL);

            tools_client_id = qobject_ref(id);
            tools_timer = g_timeout_add(TOOLS_TIMEOUT_MS, tools_timeout,
                                        NULL);
            vm_request(vm, CALL_TOOLS, NULL, msg);
            return;
        }
    }
    tools_reply(id, tools_builtin());
}

/* vm-list and choosing the VM */

static QDict *vm_describe(Vm *vm, bool reachable)
{
    QDict *d = qdict_new();

    qdict_put_str(d, "name", vm->name);
    qdict_put_int(d, "pid", vm->pid);
    qdict_put_str(d, "cwd", vm->cwd);
    qdict_put_str(d, "machine", vm->machine);
    qdict_put_str(d, "started", vm->started);
    qdict_put_bool(d, "by-agent", vm->by_agent);
    if (my_session && *vm->session) {
        qdict_put_bool(d, "this-session", !strcmp(vm->session, my_session));
    }
    qdict_put_bool(d, "reachable", reachable);
    return d;
}

static void vm_line(GString *s, Vm *vm, bool reachable)
{
    g_string_append_printf(s, "\n  %s (pid %d): %s in %s, started %s%s%s",
                           vm->name, vm->pid, vm->machine, vm->cwd,
                           vm->started,
                           vm->by_agent ? ", started by an agent"
                                        : ", the user's",
                           reachable ? "" : ", not answering");
}

static void client_vm_list(QObject *id)
{
    g_autoptr(GPtrArray) list = registry_scan();
    g_autoptr(GString) s = g_string_new(NULL);
    QDict *json = qdict_new();
    QList *arr = qlist_new();
    guint i;

    g_string_printf(s, "%u VM%s running", list->len,
                    list->len == 1 ? "" : "s");
    for (i = 0; i < list->len; i++) {
        Vm *vm = g_ptr_array_index(list, i);
        bool reachable = vm_connect(vm, NULL);

        vm_line(s, vm, reachable);
        qlist_append(arr, vm_describe(vm, reachable));
    }
    qdict_put(json, "vms", arr);
    client_tool_result(id, s->str, QOBJECT(json), false);
}

static void vm_choice_error(QObject *id, GPtrArray *list, const char *why)
{
    g_autoptr(GString) s = g_string_new(why);
    guint i;

    if (list->len) {
        g_string_append(s, "\nRunning VMs:");
    }
    for (i = 0; i < list->len; i++) {
        vm_line(s, g_ptr_array_index(list, i), true);
    }
    client_tool_result(id, s->str, NULL, true);
}

/* The VM that @arg names, or the default one; NULL after an error reply */
static Vm *vm_choose(QObject *id, QObject *arg)
{
    g_autoptr(GPtrArray) list = registry_scan();
    QString *qs = qobject_to(QString, arg);
    QNum *qn = qobject_to(QNum, arg);
    const char *name = qs ? qstring_get_str(qs) : NULL;
    int64_t pid = -1;
    Vm *found = NULL;
    guint i, n = 0;

    if (!arg) {
        if (list->len == 0) {
            vm_choice_error(id, list, "no lemu VM is running");
            return NULL;
        }
        if (list->len > 1) {
            vm_choice_error(id, list,
                            "several VMs are running; say which with vm");
            return NULL;
        }
        found = g_ptr_array_index(list, 0);
        if (!found->by_agent) {
            vm_choice_error(id, list,
                            "the only running VM was not started by an"
                            " agent, so it is the user's; pass vm only if"
                            " the user asked you to use it");
            return NULL;
        }
        return found;
    }

    if (qn && qnum_get_try_int(qn, &pid) && pid >= 0) {
        /* a PID */
    } else if (name && *name && strspn(name, "0123456789") == strlen(name)) {
        pid = g_ascii_strtoll(name, NULL, 10);
    } else if (!name) {
        vm_choice_error(id, list, "vm must be a VM name or PID");
        return NULL;
    }

    for (i = 0; i < list->len; i++) {
        Vm *vm = g_ptr_array_index(list, i);

        if (pid >= 0 ? vm->pid == pid : !strcmp(vm->name, name)) {
            found = vm;
            n++;
        }
    }
    if (n == 1) {
        return found;
    }
    {
        g_autofree char *why = n
            ? g_strdup_printf("several VMs are named '%s'; use the PID",
                              name)
            : pid >= 0 ? g_strdup_printf("no lemu VM has PID %" PRId64, pid)
                       : g_strdup_printf("no lemu VM is named '%s'", name);

        vm_choice_error(id, list, why);
    }
    return NULL;
}

static void client_tools_call(QObject *id, QDict *msg)
{
    QDict *params = qdict_get_qdict(msg, "params");
    const char *name = params ? qdict_get_try_str(params, "name") : NULL;
    QDict *args = params ? qdict_get_qdict(params, "arguments") : NULL;
    QObject *arg = args ? qdict_get(args, "vm") : NULL;
    Error *err = NULL;
    Vm *vm;

    if (!name) {
        client_reply_error(id, MCP_INVALID_PARAMS, "missing tool name",
                           NULL);
        return;
    }
    if (!strcmp(name, "vm-list")) {
        client_vm_list(id);
        return;
    }

    vm = vm_choose(id, arg);
    if (!vm) {
        return;
    }
    if (!vm_connect(vm, &err)) {
        g_autofree char *text = g_strdup_printf(
            "cannot reach VM %s (pid %d): %s", vm->name, vm->pid,
            error_get_pretty(err));

        error_free(err);
        client_tool_result(id, text, NULL, true);
        return;
    }
    if (arg) {
        qdict_del(args, "vm");
    }
    vm_request(vm, CALL_CLIENT, id, msg);
}

static void client_cancelled(QDict *params)
{
    QObject *id = params ? qdict_get(params, "requestId") : NULL;
    GHashTableIter vi, ci;
    Vm *vm;
    Call *call;

    if (!id) {
        return;
    }
    g_hash_table_iter_init(&vi, vms);
    while (g_hash_table_iter_next(&vi, NULL, (gpointer *)&vm)) {
        g_hash_table_iter_init(&ci, vm->calls);
        while (g_hash_table_iter_next(&ci, NULL, (gpointer *)&call)) {
            QDict *p;
            g_autoptr(QDict) note = NULL;
            g_autofree char *line = NULL;

            if (call->kind != CALL_CLIENT ||
                !qobject_is_equal(call->client_id, id)) {
                continue;
            }
            if (!call->line) {
                /* already sent: tell the VM */
                p = qdict_new();
                qdict_put_int(p, "requestId", call->vm_id);
                note = request("notifications/cancelled", p);
                line = message_line(note);
            }
            g_hash_table_iter_remove(&ci);
            if (line) {
                vm_write(vm, line);
            }
            return;
        }
    }
}

static void client_initialize(QObject *id, QDict *params)
{
    const char *asked = params ? qdict_get_try_str(params, "protocolVersion")
                               : NULL;
    QDict *result = qdict_new();
    QDict *caps = qdict_new();
    QDict *tools = qdict_new();
    QDict *info = qdict_new();
    g_autofree char *text = g_strconcat(mcp_instructions,
                                        bridge_instructions, NULL);

    qdict_put_str(result, "protocolVersion",
                  mcp_protocol_supported(asked) ? asked
                                                : MCP_PROTOCOL_LATEST);
    qdict_put_bool(tools, "listChanged", false);
    qdict_put(caps, "tools", tools);
    qdict_put(result, "capabilities", caps);
    qdict_put_str(info, "name", "lemu");
    qdict_put_str(info, "version", QEMU_VERSION "-mcp");
    qdict_put(result, "serverInfo", info);
    qdict_put_str(result, "instructions", text);
    client_initialized = true;
    client_reply(id, QOBJECT(result));
}

static void client_message_in(const char *line)
{
    Error *err = NULL;
    QObject *obj = qobject_from_json(line, &err);
    g_autoptr(QDict) msg = qobject_to(QDict, obj);
    QDict *params;
    QObject *id;
    const char *method, *version;

    if (err) {
        client_reply_error(NULL, MCP_PARSE_ERROR, error_get_pretty(err),
                           NULL);
        error_free(err);
        return;
    }
    if (!msg) {
        client_reply_error(NULL, MCP_INVALID_REQUEST,
                           obj && qobject_type(obj) == QTYPE_QLIST
                           ? "batches are not supported"
                           : "a message must be a JSON object", NULL);
        qobject_unref(obj);
        return;
    }

    method = qdict_get_try_str(msg, "method");
    id = qdict_get(msg, "id");
    params = qdict_get_qdict(msg, "params");
    if (!method) {
        return;                 /* a response; we never send requests */
    }
    if (!id) {
        if (!strcmp(method, "notifications/cancelled")) {
            client_cancelled(params);
        }
        return;
    }

    version = mcp_meta_protocol(params);
    if (!client_initialized && version && !mcp_protocol_supported(version)) {
        QDict *data = qdict_new();

        qdict_put(data, "supported", mcp_protocol_list());
        qdict_put_str(data, "requested", version);
        client_reply_error(id, MCP_UNSUPPORTED_VERSION,
                           "Unsupported protocol version", QOBJECT(data));
        return;
    }

    if (!strcmp(method, "initialize")) {
        client_initialize(id, params);
    } else if (!strcmp(method, "ping")) {
        client_reply(id, QOBJECT(qdict_new()));
    } else if (!strcmp(method, "tools/list")) {
        client_tools_list(id);
    } else if (!strcmp(method, "tools/call")) {
        client_tools_call(id, msg);
    } else {
        g_autofree char *text = g_strdup_printf("unknown method '%s'",
                                                method);

        client_reply_error(id, MCP_METHOD_NOT_FOUND, text, NULL);
    }
}

static gboolean client_readable(int fd, GIOCondition cond, gpointer opaque)
{
    char buf[65536];
    ssize_t n;
    char *nl;

    n = read(fd, buf, sizeof(buf));
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
        return G_SOURCE_CONTINUE;
    }
    if (n <= 0) {
        /* the client closed stdin: it is done with us */
        g_main_loop_quit(loop);
        return G_SOURCE_REMOVE;
    }
    g_string_append_len(client_in, buf, n);
    while ((nl = memchr(client_in->str, '\n', client_in->len))) {
        g_autofree char *line = g_strndup(client_in->str,
                                          nl - client_in->str);

        g_string_erase(client_in, 0, nl - client_in->str + 1);
        if (*line && strspn(line, " \t\r") != strlen(line)) {
            client_message_in(line);
        }
    }
    if (client_in->len > LINE_MAX_BYTES) {
        g_string_truncate(client_in, 0);
        client_reply_error(NULL, MCP_PARSE_ERROR, "message too long", NULL);
    }
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        printf("Usage: %s\n"
               "An MCP server on stdin and stdout for all running lemu"
               " VMs.\n"
               "Add it to Claude Code with:\n"
               "  claude mcp add --scope user --transport stdio lemu -- %s\n",
               argv[0], argv[0]);
        return strcmp(argv[1], "-h") && strcmp(argv[1], "--help");
    }

    error_init(argv[0]);
    signal(SIGPIPE, SIG_IGN);

    /* QEMU uses /tmp itself, not $TMPDIR */
    registry_dir = g_strdup_printf("/tmp/lemu-%u", (unsigned)getuid());
    my_session = g_getenv("CLAUDE_CODE_SESSION_ID");
    vms = g_hash_table_new_full(NULL, NULL, NULL, vm_free);
    client_in = g_string_new(NULL);
    loop = g_main_loop_new(NULL, false);

    g_unix_fd_add(STDIN_FILENO, G_IO_IN | G_IO_HUP | G_IO_ERR,
                  client_readable, NULL);
    g_main_loop_run(loop);
    return 0;
}
