# Using the LEMU MCP server

LEMU has a built-in [MCP](https://modelcontextprotocol.io/) server.
With it, an AI agent can work a VM like a person at the keyboard:

- look at the screen
- type and use the mouse
- read and write serial ports
- change CDs and floppies
- power the VM on and off, and take snapshots

It works on macOS and Linux.

## How it works

- **Every LEMU VM offers MCP.** It is on by default (`-mcp on`). The VM
  opens a socket in `/tmp/lemu-$UID/`, a folder only your user can open.
- **`lemu-mcp` is the bridge.** It is the one MCP server your agent starts.
  It finds all running VMs and passes each tool call to the VM you name.
- **VMs can come and go.** `lemu-mcp` keeps running.

## Setup

1. Build LEMU (see [README.md](README.md)). The bridge is
   `build/tools/lemu-mcp/lemu-mcp`. The ready-to-run kits do not include
   the bridge yet.
2. Add the bridge to your agent once.
   - Claude Code:
     `claude mcp add lemu -s user -- /path/to/build/tools/lemu-mcp/lemu-mcp`
   - Other MCP clients: add a stdio server. The command is the path to
     `lemu-mcp`, with no arguments.
3. Start a VM from a shell, for example a kit's `./run.sh`. Add
   `-name myvm` to give it a short name.
4. In the agent: run `vm-list`, then `vm-info` and `screenshot`.

**Headless works.** Start the VM with `-display none` and the agent still
sees the screen.

## Tools

Every tool takes `vm`: a name or PID from `vm-list`. You may leave it out
only when exactly one VM is running and you started it.

| Tool | What it does |
|---|---|
| `vm-list` | List the running VMs: name, PID, folder, machine, who started it |
| `vm-info` | Describe one VM: screens, mouse, disks, serial ports. Use it first |
| `screenshot` | Take a picture of a screen (PNG) |
| `frames` | List recent screens, to find one that went by too fast |
| `wait-screen` | Wait until the screen changes, settles or shows a colour. Use it instead of sleeping |
| `screen-text` | Read a text-mode screen (BIOS, DOS, NT setup) as text |
| `key` | Press keys and key combinations |
| `type-text` | Type a string, in the guest's keyboard layout |
| `mouse-move`, `mouse-click`, `mouse-drag`, `mouse-button`, `mouse-wheel` | Use the mouse, also in guests with a relative mouse |
| `pointer-locate` | Find where the guest's mouse pointer really is |
| `serial-read`, `serial-write`, `serial-expect` | Read, write and wait for serial port text |
| `media` | List, insert or eject CDs and floppies |
| `vm-power` | Pause, resume, reset, power down or quit |
| `vm-snapshot` | Save, load, list or delete whole-VM snapshots (needs qcow2 disks) |
| `hmp`, `qmp` | Run any QEMU monitor command, for experts |

## How to drive a VM

The server sends these rules to every agent that connects:

- **Look before you act.** Take a screenshot first. The keyboard and
  mouse tools take `then`, so they can wait and look again in the same
  call.
- **Don't sleep.** Use `wait-screen`. For long installs, call it in a
  loop.
- **Check the focus before typing.** Keys go to the focused window.
- **Old dialogs:** use Tab and Shift+Tab, then Space or Enter.
- **Mouse:** most old guests have a relative mouse. If a click misses,
  take a screenshot with `pointer=true`, or use the keyboard.
- **Old menus** may need press-and-hold: `mouse-button` down,
  `mouse-move`, `mouse-button` up.
- **Keyboard layout:** give `type-text` the guest's `layout`, for example
  `de` for German.
- **Events:** every result lists what happened in the VM since your last
  call, such as a reset or a pause.

## Safety

- **Other people's VMs.** `vm-list` shows which VMs an agent started
  (detected for Claude Code). All other VMs belong to the user: don't
  stop, reset or change them unless the user asks.
- **Disks.** `vm-info` shows whether disks are read-only or snapshot. If
  changes will stay, ask before risky steps, or save a `vm-snapshot`
  first.
- **Turning it off.** Start a VM with `-mcp off` to turn MCP off. MCP is
  also off under `-qtest`.

## Troubleshooting

- **A VM is missing from `vm-list`.** It was started with `-mcp off` or
  under `-qtest`, or by another user. Each VM writes
  `/tmp/lemu-$UID/<pid>.json`; check that the file is there.
- **Your own socket.** `-mcp unix:vm.mcp,server=on,wait=off` gives a VM its
  own socket instead of the default one. `lemu-mcp` still finds it. On
  macOS, Unix socket paths must stay under about 104 bytes.
