gen = decodetree.process('insns.decode')

alpha_ss = ss.source_set()
alpha_ss.add(gen)
alpha_ss.add(files(
  'cpu.c',
  'fpu_helper.c',
  'gdbstub.c',
  'helper.c',
  'op_helper.c',
  'int_helper.c',
  'tlb_helper.c',
  'translate.c',
  'monitor.c',
  'vax_helper.c',
))

alpha_system_ss = ss.source_set()
alpha_system_ss.add(files(
  'alpha-qmp-cmds.c',
  'arch_dump.c',
  'machine.c',
))

target_arch += {'alpha': alpha_ss}
target_system_arch += {'alpha': alpha_system_ss}
