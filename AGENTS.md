# Agent guide for LEMU

LEMU is a fork of QEMU for old computers. It is made to be worked on and
used by AI agents, so AI-written code is welcome here.

## Using LEMU VMs

- **Read [MCP.md](MCP.md).** The MCP server also sends its rules to every
  agent that connects.
- **Only touch your own VMs.** Never stop, reset or change a VM you did
  not start, unless the user asks.
- **Work on copies.** Test with copies of disk images and NVRAM files
  (`cp -c` on macOS, `cp --reflink` on Linux), never with the originals.
- **Start VMs from a shell.** There is no tool to start a VM. On macOS,
  keep Unix socket paths under about 104 bytes: use short relative names.

## Working on LEMU

- **Branch `master` only.** The machine branches are being folded into
  `master`. Don't commit to them.
- **One folder per machine:** `machines/<machine>/` holds notes, guest
  tools, a `run.sh` per guest system and the release scripts. Don't add
  new files to `contrib/`; upstream's own folders there stay.
- **Build:** `./configure`, then `make`. Python 3.12 or newer is needed.
  With `--without-default-features`, also give `--enable-hmp`, or
  `-monitor stdio` will not work.
- **Commit messages:** `subsystem: lowercase summary`, then a short body
  in plain English. No `Signed-off-by` or `Co-Authored-By` lines.
- **Ask first** before you push, delete branches or change GitHub
  settings.
- **Before a release,** run the feature check (see
  `machines/rm200/release/README.md`). A guest that boots can still be
  missing a window, its sound or the git id.
- **Finding your way in the QEMU code:** use the skill in
  `.agents/skills/qemu-codebase`.

## Upstream QEMU

- LEMU code never goes to QEMU. QEMU does not accept AI-generated code;
  see `docs/devel/code-provenance.rst`.
- Security bugs in upstream QEMU code follow QEMU's own process
  (`docs/system/security.rst`). Don't report them in public.
