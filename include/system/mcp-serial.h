/*
 * Serial ports for MCP clients
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SYSTEM_MCP_SERIAL_H
#define SYSTEM_MCP_SERIAL_H

#include "qapi/qapi-builtin-types.h"

/* Tap every device's chardev once the machine is up */
void mcp_serial_init(void);
/* "serial0: label (backend type)" for each tapped port */
void mcp_serial_list(strList **tail);

#endif
