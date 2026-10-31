/*
 * lemu VM registry: lets an MCP bridge find the running VMs
 *
 * Every VM gets /tmp/lemu-$UID/<pid>.sock, an MCP monitor, and
 * <pid>.json, which says what the VM is and where its other sockets
 * are.  The directory is checked as tmux checks its own: owned by the
 * user, mode 0700, not a symlink.  Only that user can connect, and that
 * user can already control the VM process.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "chardev/char.h"
#include "hw/core/boards.h"
#include "monitor/mcp.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-char.h"
#include "qemu/config-file.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/notify.h"
#include "qemu/option.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qobject/qnull.h"
#include "qobject/qstring.h"
#include "system/registry.h"
#include "system/system.h"

#define REGISTRY_MCP_ID     "lemu-mcp"

static GPtrArray *registry_endpoints;
static char *registry_dir;
static char *registry_socket;
static char *registry_json;
static char *registry_binary;
static char *registry_started;
static Notifier registry_init_done;
static Notifier registry_exit;

void lemu_registry_add_endpoint(const char *spec)
{
    if (!registry_endpoints) {
        registry_endpoints = g_ptr_array_new_with_free_func(g_free);
    }
    g_ptr_array_add(registry_endpoints, g_strdup(spec));
}

#ifdef CONFIG_POSIX

static bool registry_make_dir(const char *dir, Error **errp)
{
    struct stat st;

    if (mkdir(dir, 0700) < 0 && errno != EEXIST) {
        error_setg_errno(errp, errno, "cannot create %s", dir);
        return false;
    }
    if (lstat(dir, &st) < 0) {
        error_setg_errno(errp, errno, "cannot check %s", dir);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        error_setg(errp, "%s is not a directory", dir);
        return false;
    }
    if (st.st_uid != getuid()) {
        error_setg(errp, "%s belongs to another user", dir);
        return false;
    }
    if (st.st_mode & 077) {
        error_setg(errp, "%s is open to other users (mode %o); "
                   "it must be 0700", dir, st.st_mode & 0777);
        return false;
    }
    return true;
}

/* Drop the files of VMs that died without cleaning up */
static void registry_drop_stale(const char *dir)
{
    g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
    const char *name;

    if (!d) {
        return;
    }
    while ((name = g_dir_read_name(d))) {
        g_autofree char *path = NULL;
        const char *end;
        long pid;

        if (qemu_strtol(name, &end, 10, &pid) < 0 || pid <= 0 ||
            pid == getpid() ||
            (strcmp(end, ".json") && strcmp(end, ".sock"))) {
            continue;
        }
        /* signal 0 only checks that the process exists */
        if (kill(pid, 0) == 0 || errno != ESRCH) {
            continue;
        }
        path = g_build_filename(dir, name, NULL);
        unlink(path);
    }
}

static char *registry_binary_path(const char *argv0)
{
    g_autofree char *found = NULL;
    char buf[PATH_MAX];

    if (!strchr(argv0, '/')) {
        found = g_find_program_in_path(argv0);
        if (found) {
            argv0 = found;
        }
    }
    if (realpath(argv0, buf)) {
        return g_strdup(buf);
    }
    return g_strdup(argv0);
}

static const char *registry_chardev_file(ChardevInfoList *chardevs,
                                         const char *label)
{
    ChardevInfoList *c;

    for (c = chardevs; label && c; c = c->next) {
        if (!strcmp(c->value->label, label)) {
            return c->value->filename;
        }
    }
    return NULL;
}

typedef struct RegistryQmp {
    ChardevInfoList *chardevs;
    QList *qmp;
} RegistryQmp;

static int registry_add_qmp(void *opaque, QemuOpts *opts, Error **errp)
{
    RegistryQmp *rq = opaque;
    const char *mode = qemu_opt_get(opts, "mode");
    const char *file = registry_chardev_file(rq->chardevs,
                                             qemu_opt_get(opts, "chardev"));

    if (!g_strcmp0(mode, "control") && file) {
        qlist_append_str(rq->qmp, file);
    }
    return 0;
}

static QObject *registry_vnc(void)
{
    QemuOptsList *list = qemu_find_opts_err("vnc", NULL);
    QemuOpts *opts = list ? QTAILQ_FIRST(&list->head) : NULL;
    const char *vnc = opts ? qemu_opt_get(opts, "vnc") : NULL;

    return vnc ? QOBJECT(qstring_from_str(vnc)) : QOBJECT(qnull());
}

static void registry_write(Notifier *n, void *data)
{
    g_autoptr(QDict) d = qdict_new();
    g_autoptr(GString) json = NULL;
    g_autofree char *cwd = g_get_current_dir();
    g_autofree char *tmp = NULL;
    g_autoptr(GError) err = NULL;
    g_autoptr(ChardevInfoList) chardevs = qmp_query_chardev(NULL);
    RegistryQmp rq;
    ChardevInfoList *c;
    const char *session = g_getenv("CLAUDE_CODE_SESSION_ID");
    QDict *endpoints = qdict_new();
    QList *qmp = qlist_new();
    QList *serial = qlist_new();
    QList *mcp = qlist_new();
    int i;

    qdict_put_int(d, "pid", getpid());
    if (qemu_name) {
        qdict_put_str(d, "name", qemu_name);
    } else {
        g_autofree char *base = g_path_get_basename(cwd);

        qdict_put_str(d, "name", base);
    }
    if (registry_socket) {
        qdict_put_str(d, "socket", registry_socket);
    } else {
        qdict_put_null(d, "socket");
    }
    qdict_put_str(d, "cwd", cwd);
    qdict_put_str(d, "binary", registry_binary);
    qdict_put_str(d, "machine", current_machine ?
                  MACHINE_GET_CLASS(current_machine)->name : "");
    qdict_put_str(d, "started", registry_started);
    qdict_put_bool(d, "by-agent", g_getenv("CLAUDECODE") && session);
    if (session) {
        qdict_put_str(d, "session", session);
    } else {
        qdict_put_null(d, "session");
    }

    for (i = 0; registry_endpoints && i < registry_endpoints->len; i++) {
        qlist_append_str(mcp, g_ptr_array_index(registry_endpoints, i));
    }
    qdict_put(endpoints, "mcp", mcp);
    rq = (RegistryQmp) { chardevs, qmp };
    qemu_opts_foreach(qemu_find_opts("mon"), registry_add_qmp, &rq, NULL);
    qdict_put(endpoints, "qmp", qmp);
    qdict_put_obj(endpoints, "vnc", registry_vnc());
    for (c = chardevs; c; c = c->next) {
        if (g_str_has_prefix(c->value->label, "serial")) {
            g_autofree char *line = g_strdup_printf("%s: %s", c->value->label,
                                                    c->value->filename);

            qlist_append_str(serial, line);
        }
    }
    qdict_put(endpoints, "serial", serial);
    qdict_put(d, "endpoints", endpoints);

    json = qobject_to_json_pretty(QOBJECT(d), true);
    g_string_append_c(json, '\n');
    /* write and rename, so a reader never sees half a file */
    tmp = g_strdup_printf("%s.tmp", registry_json);
    if (!g_file_set_contents_full(tmp, json->str, json->len,
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600,
                                  &err) ||
        rename(tmp, registry_json) < 0) {
        warn_report("MCP registry: cannot write %s: %s", registry_json,
                    err ? err->message : g_strerror(errno));
        unlink(tmp);
    }
}

static void registry_cleanup(Notifier *n, void *data)
{
    if (registry_json) {
        unlink(registry_json);
    }
    if (registry_socket) {
        unlink(registry_socket);
    }
}

void lemu_registry_init(bool default_socket, const char *argv0,
                        Error **errp)
{
    ERRP_GUARD();
    g_autoptr(GDateTime) now = g_date_time_new_now_utc();

    registry_dir = g_strdup_printf("/tmp/lemu-%u", (unsigned)getuid());
    if (!registry_make_dir(registry_dir, errp)) {
        return;
    }
    registry_drop_stale(registry_dir);
    registry_json = g_strdup_printf("%s/%d.json", registry_dir, getpid());

    if (default_socket) {
        g_autofree char *spec = NULL;
        g_autofree char *path = g_strdup_printf("%s/%d.sock", registry_dir,
                                                getpid());
        QemuOpts *opts;

        /* a dead VM with our PID may have left it */
        unlink(path);
        spec = g_strdup_printf("socket,id=" REGISTRY_MCP_ID
                               ",path=%s,server=on,wait=off", path);
        opts = qemu_opts_parse(qemu_find_opts("chardev"), spec, true, errp);
        if (!opts || !qemu_chr_new_from_opts(opts, NULL, errp)) {
            return;
        }
        monitor_new_mcp(REGISTRY_MCP_ID, REGISTRY_MCP_ID, errp);
        if (*errp) {
            return;
        }
        registry_socket = g_steal_pointer(&path);
    }

    registry_binary = registry_binary_path(argv0);
    registry_started = g_date_time_format(now, "%Y-%m-%dT%H:%M:%SZ");
    registry_init_done.notify = registry_write;
    qemu_add_machine_init_done_notifier(&registry_init_done);
    registry_exit.notify = registry_cleanup;
    qemu_add_exit_notifier(&registry_exit);
}

#else

void lemu_registry_init(bool default_socket, const char *argv0,
                        Error **errp)
{
    error_setg(errp, "the MCP registry is not supported on this host yet");
}

#endif
