/*
 * MCP tool table, generated from the QAPI schema
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef MONITOR_MCP_TOOLS_H
#define MONITOR_MCP_TOOLS_H

typedef struct McpToolDef {
    const char *name;           /* tool name = QAPI command name */
    bool read_only;
    const char *json;           /* the tool definition for tools/list */
} McpToolDef;

/* Ends with an entry whose name is NULL */
extern const McpToolDef mcp_tools[];

#endif
