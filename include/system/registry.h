/*
 * lemu VM registry: lets an MCP bridge find the running VMs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SYSTEM_REGISTRY_H
#define SYSTEM_REGISTRY_H

/* Note an explicit -mcp endpoint, for the registry entry */
void lemu_registry_add_endpoint(const char *spec);

/*
 * Make /tmp/lemu-$UID, drop entries of dead VMs, and register this VM.
 * With @default_socket, also open the MCP monitor on <pid>.sock.  The
 * entry is written once the machine is up and removed at exit.
 */
void lemu_registry_init(bool default_socket, const char *argv0,
                        Error **errp);

#endif
