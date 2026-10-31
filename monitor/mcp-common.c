/*
 * MCP: what the MCP monitor and the lemu-mcp bridge share
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "monitor/mcp-common.h"
#include "qobject/qstring.h"

static const char *const mcp_protocols[] = {
    "2025-11-25", "2025-06-18", NULL
};

const char mcp_instructions[] =
    "You are operating a real emulated computer through lemu (QEMU).\n"
    "- Look before you act: take a screenshot first. After an action, use"
    " the tool's \"then\" to wait and look in the same call.\n"
    "- Use wait-screen instead of sleeping. Long installs:"
    " change-then-stable with a long stable-ms, in a loop of calls. If a"
    " clock or a graph keeps the screen from settling, add"
    " ignore-animation (not while text scrolls).\n"
    "- Check focus before typing (focus ring, highlighted title bar,"
    " caret). Keys go to the focused window, not to the one you are"
    " looking at.\n"
    "- In old dialogs prefer Tab / Shift+Tab, then Space or Enter. Alt"
    " mnemonics can be ambiguous (Alt+C may mean Continue or Cancel).\n"
    "- Combo boxes: use Home, End and arrow keys; type-ahead moves one"
    " item per key.\n"
    "- These guests mostly have relative mice. The mouse tools home the"
    " pointer and move in small steps. If a click misses, take a"
    " screenshot with pointer=true, or use the keyboard. If the pointer"
    " lands too far, the guest accelerates even small moves: move with"
    " mouse-move step-px 1, then click without x and y.\n"
    "- Old menus may need press-and-hold: mouse-button down, mouse-move,"
    " mouse-button up.\n"
    "- Use type-text with the guest's keyboard layout (for example \"de\""
    " for a German keyboard). Unknown characters are rejected before"
    " anything is typed.\n"
    "- Each result lists VM events since your last call, repeats merged"
    " with a count. A reset or stop there means the guest rebooted or the"
    " VM paused.\n"
    "- vm-info tells you whether disks are snapshot or read-only. If"
    " changes will persist, ask the user before risky steps.\n"
    "- VMs not started by you belong to the user. Touch them only when"
    " the user asked. Never stop or reset them.\n";

bool mcp_protocol_supported(const char *v)
{
    int i;

    for (i = 0; v && mcp_protocols[i]; i++) {
        if (!strcmp(v, mcp_protocols[i])) {
            return true;
        }
    }
    return false;
}

QList *mcp_protocol_list(void)
{
    QList *list = qlist_new();
    int i;

    for (i = 0; mcp_protocols[i]; i++) {
        qlist_append_str(list, mcp_protocols[i]);
    }
    return list;
}

const char *mcp_meta_protocol(QDict *params)
{
    QDict *meta = params ? qdict_get_qdict(params, "_meta") : NULL;
    const QDictEntry *e;

    if (!meta) {
        return NULL;
    }
    for (e = qdict_first(meta); e; e = qdict_next(meta, e)) {
        QString *s = qobject_to(QString, qdict_entry_value(e));

        if (s && strstr(qdict_entry_key(e), "protocolVersion")) {
            return qstring_get_str(s);
        }
    }
    return NULL;
}
