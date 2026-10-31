/*
 * Serial ports for MCP clients (see qapi/mcp.json)
 *
 * Every chardev frontend of a device gets a tap once the machine is up,
 * so what the guest writes to its serial ports can be read back without
 * changing the command line, even behind mon:stdio.  Input goes straight
 * to the device's own read handlers, past any mux.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "chardev/char-tap.h"
#include "chardev/char.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/qdev.h"
#include "monitor/mcp.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-mcp.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "system/mcp-serial.h"
#include "system/runstate.h"
#include "system/system.h"
#include "ui/input.h"

#define SERIAL_TAP_SIZE     (1024 * 1024)
#define SERIAL_READ_MAX     16384
#define SERIAL_WAIT_MAX_MS  110000
#define SERIAL_STALL_MS     10000

typedef struct McpSerialPort {
    CharFrontend *fe;
    CharTap *tap;
    uint64_t mark;              /* where the last read or expect stopped */
} McpSerialPort;

static GPtrArray *serial_ports;
static Notifier serial_init_done;

static gint mcp_serial_cmp(gconstpointer a, gconstpointer b)
{
    const McpSerialPort *pa = *(McpSerialPort * const *)a;
    const McpSerialPort *pb = *(McpSerialPort * const *)b;

    return g_strcmp0(qemu_chr_tap_name(pa->tap), qemu_chr_tap_name(pb->tap));
}

static void mcp_serial_add(CharFrontend *fe)
{
    g_autofree char *name = NULL;
    McpSerialPort *port;
    int i;

    for (i = 0; serial_hd(i); i++) {
        if (serial_hd(i) == fe->chr) {
            name = g_strdup_printf("serial%d", i);
            break;
        }
    }
    if (!name) {
        name = g_strdup(fe->chr->label);
    }
    port = g_new0(McpSerialPort, 1);
    port->fe = fe;
    port->tap = qemu_chr_tap_new(name, SERIAL_TAP_SIZE);
    qemu_chr_fe_set_tap(fe, port->tap);
    g_ptr_array_add(serial_ports, port);
}

static int mcp_serial_scan(Object *obj, void *opaque)
{
    ObjectClass *oc;

    if (!object_dynamic_cast(obj, TYPE_DEVICE)) {
        return 0;
    }
    for (oc = object_get_class(obj);
         oc && object_class_dynamic_cast(oc, TYPE_DEVICE);
         oc = object_class_get_parent(oc)) {
        DeviceClass *dc = DEVICE_CLASS(oc);
        int i;

        for (i = 0; i < dc->props_count_; i++) {
            const Property *prop = &dc->props_[i];
            CharFrontend *fe;

            if (prop->info != &qdev_prop_chr) {
                continue;
            }
            fe = object_field_prop_ptr(obj, prop);
            if (fe->chr && !fe->tap) {
                mcp_serial_add(fe);
            }
        }
    }
    return 0;
}

static void mcp_serial_machine_done(Notifier *n, void *data)
{
    object_child_foreach_recursive(object_get_root(), mcp_serial_scan, NULL);
    g_ptr_array_sort(serial_ports, mcp_serial_cmp);
}

void mcp_serial_init(void)
{
    serial_ports = g_ptr_array_new();
    serial_init_done.notify = mcp_serial_machine_done;
    qemu_add_machine_init_done_notifier(&serial_init_done);
}

void mcp_serial_list(strList **tail)
{
    int i;

    for (i = 0; serial_ports && i < serial_ports->len; i++) {
        McpSerialPort *port = g_ptr_array_index(serial_ports, i);
        Chardev *chr = port->fe->chr;
        g_autofree char *backend = NULL;

        backend = chr ? g_strdup_printf("%s (%s)", chr->label,
                                        object_get_typename(OBJECT(chr)))
                      : g_strdup("none");
        QAPI_LIST_APPEND(tail, g_strdup_printf("%s: %s",
                                               qemu_chr_tap_name(port->tap),
                                               backend));
    }
}

static McpSerialPort *mcp_serial_find(const char *name, Error **errp)
{
    g_autoptr(GString) names = g_string_new(NULL);
    int i;

    name = name ? name : "serial0";
    for (i = 0; serial_ports && i < serial_ports->len; i++) {
        McpSerialPort *port = g_ptr_array_index(serial_ports, i);

        if (!strcmp(qemu_chr_tap_name(port->tap), name)) {
            return port;
        }
        g_string_append_printf(names, "%s%s", i ? ", " : "",
                               qemu_chr_tap_name(port->tap));
    }
    error_setg(errp, "no serial port '%s'; ports: %s", name,
               names->len ? names->str : "none");
    return NULL;
}

/*
 * Turn raw serial bytes into UTF-8 text.  @map[i] is the raw offset
 * (from @raw) just after text byte i, so a match can be turned back
 * into a cursor.
 */
static GString *mcp_serial_text(const uint8_t *raw, size_t len, bool strip,
                                GArray *map)
{
    GString *s = g_string_sized_new(len);
    size_t i = 0;

#define EMIT(c, next) do {                                          \
        uint32_t end_ = (next);                                     \
        g_string_append_c(s, (c));                                  \
        if (map) {                                                  \
            g_array_append_val(map, end_);                          \
        }                                                           \
    } while (0)

    while (i < len) {
        uint8_t c = raw[i];

        if (strip && c == 0x1b) {
            /* CSI: ESC [ params final; OSC: ESC ] ... BEL or ESC \ */
            size_t j = i + 1;

            if (j < len && raw[j] == '[') {
                for (j++; j < len && (raw[j] < 0x40 || raw[j] > 0x7e); j++) {
                    /* parameters */
                }
                i = MIN(j + 1, len);
            } else if (j < len && raw[j] == ']') {
                for (j++; j < len && raw[j] != 0x07 &&
                     !(raw[j] == 0x1b && j + 1 < len && raw[j + 1] == '\\');
                     j++) {
                    /* text */
                }
                i = MIN(j + (j < len && raw[j] == 0x1b ? 2 : 1), len);
            } else {
                i = MIN(j + 1, len);
            }
            continue;
        }
        if (strip && (c == '\r' || c == 0 || c == 0x7f ||
                      (c < 0x20 && c != '\n' && c != '\t' && c != '\b'))) {
            i++;
            continue;
        }
        if (c < 0x80) {
            EMIT(c, i + 1);
            i++;
        } else {
            const gchar *end;
            size_t n = MIN(len - i, 4);

            /* keep a valid UTF-8 sequence, else show U+FFFD */
            g_utf8_validate((const gchar *)raw + i, n, &end);
            n = (const uint8_t *)end - (raw + i);
            /* invalid input gives (gunichar)-1 or -2, above U+10FFFF */
            if (n && g_utf8_get_char_validated((const gchar *)raw + i, n)
                     <= 0x10ffff) {
                size_t k;

                for (k = 0; k < n; k++) {
                    EMIT(raw[i + k], i + n);
                }
                i += n;
            } else {
                EMIT(0xef, i + 1);
                EMIT(0xbf, i + 1);
                EMIT(0xbd, i + 1);
                i++;
            }
        }
    }
#undef EMIT
    return s;
}

McpSerialResult *qmp_serial_read(const char *port_name, bool has_cursor,
                                 int64_t cursor, bool has_max_bytes,
                                 int64_t max_bytes, bool has_strip_ansi,
                                 bool strip_ansi, Error **errp)
{
    McpSerialPort *port = mcp_serial_find(port_name, errp);
    g_autofree uint8_t *buf = NULL;
    g_autoptr(GString) text = NULL;
    McpSerialResult *res;
    uint64_t from, actual, start, end;
    size_t n, max;

    if (!port) {
        return NULL;
    }
    max = has_max_bytes ? MIN(MAX(max_bytes, 1), SERIAL_READ_MAX) : 4096;
    from = has_cursor ? MAX(cursor, 0) : port->mark;
    qemu_chr_tap_range(port->tap, &start, &end);
    buf = g_malloc(max);
    n = qemu_chr_tap_read(port->tap, from, buf, max, &actual);
    text = mcp_serial_text(buf, n, has_strip_ansi ? strip_ansi : true, NULL);
    port->mark = actual + n;

    res = g_new0(McpSerialResult, 1);
    res->port = g_strdup(qemu_chr_tap_name(port->tap));
    res->start = actual;
    res->cursor = actual + n;
    if (actual > from) {
        res->has_lost = true;
        res->lost = actual - from;
    }
    res->summary = g_strdup_printf("%s: %zu bytes from %" PRIu64
                                   "%s, cursor %" PRIu64 "%s%s",
                                   res->port, n, actual,
                                   actual + n < end ? " (more to read)" : "",
                                   actual + n,
                                   n ? ":\n" : "", text->str);
    return res;
}

/* Turn \r, \n, \t, \e, \\ and \xHH into characters; others stay */
static GString *mcp_serial_unescape(const char *text)
{
    GString *s = g_string_new(NULL);
    const char *p = text;

    while (*p) {
        if (p[0] != '\\' || !p[1]) {
            g_string_append_c(s, *p++);
            continue;
        }
        switch (p[1]) {
        case 'r':
            g_string_append_c(s, '\r');
            p += 2;
            break;
        case 'n':
            g_string_append_c(s, '\n');
            p += 2;
            break;
        case 't':
            g_string_append_c(s, '\t');
            p += 2;
            break;
        case 'e':
            g_string_append_c(s, 0x1b);
            p += 2;
            break;
        case '\\':
            g_string_append_c(s, '\\');
            p += 2;
            break;
        case 'x':
            if (g_ascii_isxdigit(p[2]) && g_ascii_isxdigit(p[3])) {
                g_string_append_c(s, g_ascii_xdigit_value(p[2]) * 16 +
                                     g_ascii_xdigit_value(p[3]));
                p += 4;
                break;
            }
            /* fall through */
        default:
            g_string_append_c(s, *p++);
            break;
        }
    }
    return s;
}

/* The input context: the MCP call's, or @local for a QMP caller */
static QemuInputCtx *mcp_serial_ctx(QemuInputCtx *local)
{
    QemuInputCtx *ctx = mcp_current_input_ctx();

    if (ctx) {
        return ctx;
    }
    qemu_input_ctx_init(local, NULL);
    return local;
}

static void coroutine_fn mcp_serial_sleep(QemuInputCtx *ctx, int64_t ms)
{
    qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME, ms * SCALE_MS);
}

McpResult *coroutine_fn qmp_serial_write(const char *port_name,
                                         const char *text, bool has_escapes,
                                         bool escapes, bool has_char_delay_ms,
                                         int64_t char_delay_ms, Error **errp)
{
    McpSerialPort *port = mcp_serial_find(port_name, errp);
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_serial_ctx(&local);
    g_autoptr(GString) data = NULL;
    int64_t delay = has_char_delay_ms ? MAX(char_delay_ms, 0) : 0;
    int64_t last_progress = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    McpResult *res;
    size_t pos = 0;

    if (!port) {
        return NULL;
    }
    if (!runstate_is_running()) {
        error_setg(errp, "VM is paused (state %s); resume it first",
                   RunState_str(runstate_get()));
        return NULL;
    }
    data = (has_escapes && !escapes) ? g_string_new(text)
                                     : mcp_serial_unescape(text);

    while (pos < data->len) {
        CharFrontend *fe = port->fe;
        int n = fe->chr_can_read ? fe->chr_can_read(fe->opaque) : 0;

        if (ctx->cancelled) {
            error_setg(errp, "cancelled after %zu of %zu bytes", pos,
                       data->len);
            return NULL;
        }
        if (n <= 0) {
            if (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - last_progress >
                SERIAL_STALL_MS) {
                error_setg(errp, "%s took only %zu of %zu bytes; the guest "
                           "is not reading the port",
                           qemu_chr_tap_name(port->tap), pos, data->len);
                return NULL;
            }
            mcp_serial_sleep(ctx, 5);
            continue;
        }
        n = delay ? 1 : MIN(n, data->len - pos);
        fe->chr_read(fe->opaque, (uint8_t *)data->str + pos, n);
        pos += n;
        last_progress = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        if (delay && pos < data->len) {
            mcp_serial_sleep(ctx, delay);
        }
    }

    res = g_new0(McpResult, 1);
    res->summary = g_strdup_printf("sent %zu bytes to %s", data->len,
                                   qemu_chr_tap_name(port->tap));
    return res;
}

typedef struct McpSerialWaiter {
    Notifier notifier;
    QemuInputCtx *ctx;
} McpSerialWaiter;

static void mcp_serial_wake(Notifier *n, void *data)
{
    McpSerialWaiter *w = container_of(n, McpSerialWaiter, notifier);

    qemu_co_sleep_wake(&w->ctx->sleep);
}

/* The last @max bytes of @s, cut at a UTF-8 character boundary */
static const char *mcp_serial_tail(GString *s, size_t max)
{
    const char *p = s->str;

    if (s->len > max) {
        p = g_utf8_find_next_char(s->str + s->len - max - 1, NULL);
    }
    return p ? p : "";
}

McpSerialResult *coroutine_fn qmp_serial_expect(const char *port_name,
                                                const char *pattern,
                                                bool has_cursor,
                                                int64_t cursor,
                                                bool has_timeout_ms,
                                                int64_t timeout_ms,
                                                Error **errp)
{
    McpSerialPort *port = mcp_serial_find(port_name, errp);
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_serial_ctx(&local);
    g_autoptr(GRegex) re = NULL;
    g_autoptr(GError) gerr = NULL;
    g_autoptr(GString) text = NULL;
    g_autoptr(GArray) map = NULL;
    g_autofree uint8_t *buf = NULL;
    g_autofree char *match = NULL;
    McpSerialWaiter w = { .notifier.notify = mcp_serial_wake, .ctx = ctx };
    int64_t start_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t deadline;
    uint64_t from, actual = 0, end_off = 0;
    McpSerialResult *res;
    bool matched = false;
    size_t n = 0;

    if (!port) {
        return NULL;
    }
    re = g_regex_new(pattern, G_REGEX_MULTILINE, 0, &gerr);
    if (!re) {
        error_setg(errp, "bad pattern: %s", gerr->message);
        return NULL;
    }
    timeout_ms = MIN(has_timeout_ms ? MAX(timeout_ms, 0) : 30000,
                     SERIAL_WAIT_MAX_MS);
    deadline = start_ms + timeout_ms;
    from = has_cursor ? MAX(cursor, 0) : port->mark;
    buf = g_malloc(SERIAL_TAP_SIZE);
    map = g_array_new(false, false, sizeof(uint32_t));

    qemu_chr_tap_add_notifier(port->tap, &w.notifier);
    for (;;) {
        g_autoptr(GMatchInfo) mi = NULL;
        int64_t now;

        n = qemu_chr_tap_read(port->tap, from, buf, SERIAL_TAP_SIZE, &actual);
        g_array_set_size(map, 0);
        if (text) {
            g_string_free(text, true);
        }
        text = mcp_serial_text(buf, n, true, map);
        if (g_regex_match(re, text->str, 0, &mi)) {
            int ms, me;

            g_match_info_fetch_pos(mi, 0, &ms, &me);
            match = g_match_info_fetch(mi, 0);
            end_off = actual + (me ? g_array_index(map, uint32_t, me - 1) : 0);
            g_string_truncate(text, me);
            matched = true;
            break;
        }
        now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        if (ctx->cancelled || now >= deadline) {
            break;
        }
        mcp_serial_sleep(ctx, deadline - now);
    }
    qemu_chr_tap_remove_notifier(&w.notifier);

    if (ctx->cancelled) {
        error_setg(errp, "cancelled");
        return NULL;
    }
    res = g_new0(McpSerialResult, 1);
    res->port = g_strdup(qemu_chr_tap_name(port->tap));
    res->start = actual;
    if (actual > from) {
        res->has_lost = true;
        res->lost = actual - from;
    }
    res->has_matched = true;
    res->matched = matched;
    if (matched) {
        port->mark = end_off;
        res->cursor = end_off;
        res->match = g_steal_pointer(&match);
        res->summary = g_strdup_printf("%s: matched after %" PRId64
                                       " ms, cursor %" PRIu64 ":\n%s",
                                       res->port,
                                       qemu_clock_get_ms(QEMU_CLOCK_REALTIME)
                                       - start_ms, end_off,
                                       mcp_serial_tail(text, 4096));
    } else {
        /* the cursor stays, so the next call sees the same text */
        res->cursor = from;
        res->summary = g_strdup_printf("%s: no match within %" PRId64
                                       " ms; %zu bytes since the cursor, "
                                       "ending with:\n%s",
                                       res->port, timeout_ms, n,
                                       mcp_serial_tail(text, 2048));
    }
    return res;
}
