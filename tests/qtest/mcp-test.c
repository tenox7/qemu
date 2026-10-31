/*
 * qtest for the MCP screen and input commands
 *
 * The input test sends 5,000 keys with the "key" command and reads them
 * back from the i8042 while stepping the clock: none may be lost, the
 * order must hold, and nothing may arrive while the VM is paused.
 *
 * The screen tests draw into the VGA linear framebuffer and check that
 * "wait-screen" sees the change, and that a blinking box does not stop
 * the screen from counting as stable.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <sys/socket.h>
#include <sys/un.h>
#include "libqtest.h"
#include "libqos/pci-pc.h"
#include "qobject/qdict.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"

#define NKEYS       5000

#define I8042_DATA      0x60
#define I8042_STATUS    0x64
#define I8042_CMD       0x64
#define I8042_OBF       0x01

/* Set 1 make codes for a..z */
static const uint8_t set1[26] = {
    0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25,
    0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f,
    0x11, 0x2d, 0x15, 0x2c,
};

static int drain_kbd(QTestState *qts, uint8_t *buf, int n, int max)
{
    while (qtest_inb(qts, I8042_STATUS) & I8042_OBF) {
        g_assert_cmpint(n, <, max);
        buf[n++] = qtest_inb(qts, I8042_DATA);
    }
    return n;
}

/* A line-based MCP client on @path */
static int mcp_connect(const char *path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    int i;

    g_assert(fd >= 0);
    g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
    for (i = 0; i < 100; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        g_usleep(10 * 1000);
    }
    g_assert_not_reached();
}

static void mcp_send(int fd, const char *line)
{
    size_t len = strlen(line);

    g_assert_cmpint(write(fd, line, len), ==, len);
    g_assert_cmpint(write(fd, "\n", 1), ==, 1);
}

static char *mcp_recv(int fd)
{
    GString *s = g_string_new(NULL);
    char c;

    while (read(fd, &c, 1) == 1 && c != '\n') {
        g_string_append_c(s, c);
    }
    return g_string_free(s, false);
}

static void test_keys(void)
{
    g_autofree char *dir = g_dir_make_tmp("mcp-test-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "mcp.sock", NULL);
    g_autoptr(GString) cmd = g_string_new(NULL);
    int max = NKEYS * 2 + 16;
    g_autofree uint8_t *got = g_new0(uint8_t, max);
    g_autofree char *line = NULL;
    QTestState *qts;
    int fd, i, n = 0, before, steps;

    /*
     * The keys go over MCP, so that QMP stays free for stop and cont
     * while the "key" command runs.
     */
    qts = qtest_initf("-machine pc -display none "
                      "-mcp unix:%s,server=on,wait=off", path);
    fd = mcp_connect(path);
    mcp_send(fd, "{\"jsonrpc\": \"2.0\", \"id\": 1, \"method\": "
             "\"initialize\", \"params\": {\"protocolVersion\": "
             "\"2025-11-25\", \"capabilities\": {}, \"clientInfo\": "
             "{\"name\": \"mcp-test\", \"version\": \"1\"}}}");
    g_free(mcp_recv(fd));

    /* scancode translation on, so we read set 1 */
    qtest_outb(qts, I8042_CMD, 0x60);
    qtest_outb(qts, I8042_DATA, 0x45);

    g_string_append(cmd, "{\"jsonrpc\": \"2.0\", \"id\": 2, \"method\": "
                    "\"tools/call\", \"params\": {\"name\": \"key\", "
                    "\"arguments\": {\"hold-ms\": 1, \"delay-ms\": 1, "
                    "\"keys\": [");
    for (i = 0; i < NKEYS; i++) {
        g_string_append_printf(cmd, "%s\"%c\"", i ? ", " : "", 'a' + i % 26);
    }
    g_string_append(cmd, "]}}}");
    mcp_send(fd, cmd->str);

    /* step part of the way, then pause: nothing may arrive while paused */
    for (steps = 0; steps < NKEYS; steps++) {
        qtest_clock_step(qts, SCALE_MS);
        n = drain_kbd(qts, got, n, max);
    }
    g_assert_cmpint(n, >, 0);
    g_assert_cmpint(n, <, NKEYS * 2);

    qtest_qmp_assert_success(qts, "{'execute': 'stop'}");
    before = n = drain_kbd(qts, got, n, max);
    for (i = 0; i < 200; i++) {
        qtest_clock_step(qts, SCALE_MS);
        n = drain_kbd(qts, got, n, max);
    }
    g_assert_cmpint(n, ==, before);
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");

    for (steps = 0; steps < NKEYS * 4 && n < NKEYS * 2; steps++) {
        qtest_clock_step(qts, SCALE_MS);
        n = drain_kbd(qts, got, n, max);
    }

    /* the call answers after the pause that follows the last key */
    for (i = 0; i < 10; i++) {
        qtest_clock_step(qts, SCALE_MS);
    }
    line = mcp_recv(fd);
    g_assert(strstr(line, "\"id\": 2"));
    g_assert(strstr(line, "\"isError\": false"));

    g_assert_cmpint(n, ==, NKEYS * 2);
    for (i = 0; i < NKEYS; i++) {
        g_assert_cmphex(got[2 * i], ==, set1[i % 26]);
        g_assert_cmphex(got[2 * i + 1], ==, set1[i % 26] | 0x80);
    }
    close(fd);
    qtest_quit(qts);
    unlink(path);
    rmdir(dir);
}

/* Screen */

#define VBE_INDEX   0x1ce
#define VBE_DATA    0x1cf
#define SCR_W       640
#define SCR_H       480

typedef struct Screen {
    QTestState *qts;
    QPCIBus *bus;
    QPCIDevice *dev;
    QPCIBar lfb;
} Screen;

static void vbe_write(QTestState *qts, uint16_t index, uint16_t val)
{
    qtest_outw(qts, VBE_INDEX, index);
    qtest_outw(qts, VBE_DATA, val);
}

static void screen_init(Screen *s)
{
    s->qts = qtest_init("-machine pc -display none -vga std");
    s->bus = qpci_new_pc(s->qts, NULL);
    s->dev = qpci_device_find(s->bus, QPCI_DEVFN(2, 0));
    g_assert(s->dev);
    qpci_device_enable(s->dev);
    s->lfb = qpci_iomap(s->dev, 0, NULL);

    vbe_write(s->qts, 1, SCR_W);        /* xres */
    vbe_write(s->qts, 2, SCR_H);        /* yres */
    vbe_write(s->qts, 3, 32);           /* bpp */
    vbe_write(s->qts, 4, 0x41);         /* enabled, linear framebuffer */
    /* attribute controller: palette address source, so not blanked */
    qtest_inb(s->qts, 0x3da);
    qtest_outb(s->qts, 0x3c0, 0x20);
}

static void screen_free(Screen *s)
{
    g_free(s->dev);
    qpci_free_pc(s->bus);
    qtest_quit(s->qts);
}

static void screen_fill(Screen *s, int x, int y, int w, int h, uint32_t rgb)
{
    g_autofree uint32_t *row = g_new(uint32_t, w);
    int i;

    for (i = 0; i < w; i++) {
        row[i] = cpu_to_le32(rgb);
    }
    for (i = 0; i < h; i++) {
        qpci_memwrite(s->dev, s->lfb, ((y + i) * SCR_W + x) * 4, row, w * 4);
    }
}

static QDict *screen_wait_reply(Screen *s)
{
    QDict *rsp;

    for (;;) {
        rsp = qtest_qmp_receive(s->qts);
        if (!qdict_haskey(rsp, "event")) {
            return rsp;
        }
        qobject_unref(rsp);
    }
}

static void test_screen_change(void)
{
    Screen s;
    QDict *rsp, *ret, *wait, *box;

    screen_init(&s);
    /* let the screen settle in the new mode */
    qtest_qmp_assert_success(s.qts, "{'execute': 'wait-screen', 'arguments':"
                             " {'until': 'stable', 'stable-ms': 300,"
                             " 'timeout-ms': 5000, 'screenshot': false}}");

    qtest_qmp_send(s.qts, "{'execute': 'wait-screen', 'arguments':"
                   " {'until': 'change', 'timeout-ms': 5000,"
                   " 'screenshot': false}}");
    g_usleep(300 * 1000);
    screen_fill(&s, 100, 50, 40, 20, 0xffffff);

    rsp = screen_wait_reply(&s);
    ret = qdict_get_qdict(rsp, "return");
    g_assert(ret);
    wait = qdict_get_qdict(ret, "wait");
    g_assert(qdict_get_bool(wait, "matched"));
    g_assert_cmpint(qdict_get_int(wait, "changed-pixels"), ==, 40 * 20);
    box = qdict_get_qdict(wait, "box");
    g_assert_cmpint(qdict_get_int(box, "x"), ==, 100);
    g_assert_cmpint(qdict_get_int(box, "y"), ==, 50);
    g_assert_cmpint(qdict_get_int(box, "w"), ==, 40);
    g_assert_cmpint(qdict_get_int(box, "h"), ==, 20);
    qobject_unref(rsp);

    /* a change outside the region does not count */
    qtest_qmp_send(s.qts, "{'execute': 'wait-screen', 'arguments':"
                   " {'until': 'change', 'timeout-ms': 1000,"
                   " 'region': {'x': 0, 'y': 300, 'w': 640, 'h': 100},"
                   " 'screenshot': false}}");
    g_usleep(300 * 1000);
    screen_fill(&s, 100, 50, 40, 20, 0x000000);
    rsp = screen_wait_reply(&s);
    ret = qdict_get_qdict(rsp, "return");
    g_assert(!qdict_get_bool(qdict_get_qdict(ret, "wait"), "matched"));
    qobject_unref(rsp);

    screen_free(&s);
}

/* Toggle a text-cursor-sized box every 150 ms during a "stable" wait */
static bool blink_wait(Screen *s, bool ignore_blink)
{
    QDict *rsp, *ret;
    bool matched;
    int i;

    qtest_qmp_send(s->qts, "{'execute': 'wait-screen', 'arguments':"
                   " {'until': 'stable', 'stable-ms': 1000,"
                   " 'timeout-ms': 3000, 'ignore-blink': %i,"
                   " 'screenshot': false}}", ignore_blink);
    for (i = 0; i < 22; i++) {
        screen_fill(s, 200, 200, 8, 16, i & 1 ? 0xc0c0c0 : 0x000000);
        g_usleep(150 * 1000);
    }
    rsp = screen_wait_reply(s);
    ret = qdict_get_qdict(rsp, "return");
    g_assert(ret);
    matched = qdict_get_bool(qdict_get_qdict(ret, "wait"), "matched");
    qobject_unref(rsp);
    return matched;
}

static void test_screen_blink(void)
{
    Screen s;

    screen_init(&s);
    qtest_qmp_assert_success(s.qts, "{'execute': 'wait-screen', 'arguments':"
                             " {'until': 'stable', 'stable-ms': 300,"
                             " 'timeout-ms': 5000, 'screenshot': false}}");
    g_assert(!blink_wait(&s, false));
    g_assert(blink_wait(&s, true));
    screen_free(&s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mcp/keys", test_keys);
    qtest_add_func("/mcp/screen/change", test_screen_change);
    qtest_add_func("/mcp/screen/blink", test_screen_blink);
    return g_test_run();
}
