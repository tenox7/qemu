/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "monitor/mcp.h"

void monitor_new_mcp(const char *id, const char *chardev_id, Error **errp)
{
    error_setg(errp, "MCP monitors are only available in system emulators");
}

QemuInputCtx *mcp_current_input_ctx(void)
{
    return NULL;
}
