qmp_ss.add(files('monitor.c', 'qmp.c', 'qmp-cmds-control.c'))

# shared with the lemu-mcp bridge
util_ss.add(files('mcp-common.c'))

system_ss.add(files(
  'fds.c',
  'mcp.c',
  'qemu-config-qmp.c',
  'qmp-cmds.c',
))

if have_hmp
  system_ss.add(files(
    'hmp-cmds.c',
    'hmp.c',
  ))
endif
