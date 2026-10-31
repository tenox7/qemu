# LEMU

LEMU is a fork of [QEMU](https://www.qemu.org/).
It emulates **old computers**, and it is made to be used **with AI agents**.

## How LEMU differs from QEMU

- **Old machines.** The focus is complete system emulation of legacy
  platforms and their operating systems. Modern systems, KVM and cloud
  virtualization are not the goal.
- **Made for AI.** QEMU does not accept AI-generated code. LEMU is the
  opposite: it is written with AI agents and built for them.
- **Built-in MCP server.** An AI agent can see the screen, type, use the
  mouse and serial ports, change disks, power the VM and take snapshots.
  See [MCP.md](MCP.md).
- **Agent guides and skills.** [AGENTS.md](AGENTS.md) is for agents that
  work on LEMU. Skills are in [.agents/skills](.agents/skills).
- **Ready-to-run kits.** One portable folder per guest: LEMU for macOS,
  Linux and Windows, the disk, the firmware and `run.sh` / `run.cmd`.
  The scripts that build the kits are in [machines/](machines).

## Machines

| Machine | Guests |
|---|---|
| Siemens Nixdorf RM200 and RM300 (MIPS) | Windows NT 4.0, SINIX-N 5.45 |
| MIPS Magnum (Jazz) | Windows NT 3.1 |
| DEC Jensen (DECpc AXP 150, Alpha) | Windows NT 3.1, Windows NT 4.0 |

Coming next, from the machine branches:

- IBM 40p PReP: AIX, Windows NT, OS/2, Solaris
- IBM pSeries: AIX
- Intel PC: Windows NT 3.1

## Build

    ./configure --target-list=mips64el-softmmu,mips64-softmmu,alpha-softmmu
    make

- You need the same tools as for QEMU, and Python 3.12 or newer.
- Leave out `--target-list` to build every target.

## Use it with an agent

1. Build LEMU. The MCP bridge is `build/tools/lemu-mcp/lemu-mcp`.
2. Add the bridge to your agent once. For Claude Code:
   `claude mcp add lemu -s user -- /path/to/build/tools/lemu-mcp/lemu-mcp`
3. Start a VM from a shell, for example a kit's `./run.sh`.
4. Ask the agent to list the VMs and take a screenshot.

The MCP server works on macOS and Linux. All the details are in
[MCP.md](MCP.md).

## Rules

- LEMU code does not go back to QEMU.
- Please don't send LEMU patches or LEMU bug reports to the QEMU project.

## License

GPL version 2, like QEMU. See [COPYING](COPYING).
QEMU's own README is [README.rst](README.rst).
