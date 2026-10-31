/*
 * MCP monitor: the Model Context Protocol, for AI agents
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef MONITOR_MCP_H
#define MONITOR_MCP_H

#include "monitor/monitor.h"
#include "ui/input.h"

#define TYPE_MONITOR_MCP "monitor-mcp"
OBJECT_DECLARE_TYPE(MonitorMCP, MonitorMCPClass, MONITOR_MCP);

void monitor_new_mcp(const char *id, const char *chardev_id, Error **errp);

/*
 * The input context of the MCP tool call running in this coroutine,
 * or NULL when the command was not called through MCP.  Input sent
 * with it is released when the client disconnects or cancels.
 */
QemuInputCtx *mcp_current_input_ctx(void);

#endif
