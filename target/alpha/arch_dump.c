/* SPDX-License-Identifier: MIT */
/*
 * QEMU Alpha CPU -- support for writing ELF notes
 */

#include "qemu/osdep.h"
#include "system/dump.h"
#include "exec/target_page.h"
#include "cpu.h"
#include "elf.h"

#define ELF_CLASS       ELFCLASS64
#define ELF_DATA        ELFDATA2LSB
#define ELF_ARCH        EM_ALPHA

/* Keep guest ELF types distinct from the host's <sys/procfs.h> types. */
typedef uint64_t        alpha_elf_reg_t;

typedef struct alpha_elf_gregset {
        alpha_elf_reg_t       r_regs[32];
} QEMU_PACKED alpha_elf_gregset_t;
QEMU_BUILD_BUG_ON(sizeof(alpha_elf_gregset_t) != 256);

typedef struct alpha_elf_fpregset {
        alpha_elf_reg_t       fpr_regs[32];
        alpha_elf_reg_t       fpr_cr;
} QEMU_PACKED alpha_elf_fpregset_t;
QEMU_BUILD_BUG_ON(sizeof(alpha_elf_fpregset_t) != 264);

typedef struct alpha_elf_prstatus {
        int32_t         pr_version;     /* version number of struct */
        int32_t         pr_pad0;
        uint64_t        pr_statussz;    /* sizeof(prstatus_t) */
        uint64_t        pr_gregsetsz;   /* sizeof(gregset_t) */
        uint64_t        pr_fpregsetsz;  /* sizeof(fpregset_t) */
        int32_t         pr_pad1;
        int32_t         pr_osreldate;   /* kernel version */
        int32_t         pr_cursig;      /* current signal */
        int32_t         pr_pid;         /* lwp (thread) ID */
        alpha_elf_gregset_t   pr_reg;         /* general purpose registers */
} QEMU_PACKED alpha_elf_prstatus_t;
QEMU_BUILD_BUG_ON(sizeof(alpha_elf_prstatus_t) != 304);

typedef struct alpha_elf_note {
        Elf64_Nhdr      n_hdr;
        char            n_name[8];      /* align_up(sizeof("FreeBSD"), 4) */
        union {
                alpha_elf_prstatus_t      un_prstatus;
                alpha_elf_fpregset_t      un_prfpregset;
        } n_un;
} QEMU_PACKED alpha_elf_note_t;

#define ELF_OSRELDATE           604000
#define ELF_PRSTATUS_VERSION    1

#define ELF_CORE_VENDOR         "FreeBSD"

#define ELF_NOTE_SIZE           offsetof(alpha_elf_note_t, n_un.un_prstatus)
#define ELF_PRSTATUS_SIZE       (ELF_NOTE_SIZE + sizeof(alpha_elf_prstatus_t))
#define ELF_PRFPREG_SIZE        (ELF_NOTE_SIZE + sizeof(alpha_elf_fpregset_t))

static inline void alpha_note_init(alpha_elf_note_t *note, DumpState *s,
                                   const char *name, Elf64_Word type,
                                   Elf64_Word descsz)
{
    int namesz = MAX(sizeof(note->n_name), strlen(name));

    memset(note, 0, sizeof(*note));
    note->n_hdr.n_namesz = cpu_to_dump32(s, namesz);
    note->n_hdr.n_descsz = cpu_to_dump32(s, descsz);
    note->n_hdr.n_type = cpu_to_dump32(s, type);
    strncpy(note->n_name, name, sizeof(note->n_name));
}

static int alpha_write_elf64_prfpreg(WriteCoreDumpFunction f,
                                     CPUAlphaState *env, int cpuid,
                                     DumpState *s)
{
    alpha_elf_fpregset_t *fpr;
    alpha_elf_note_t note;
    int ret, i;

    alpha_note_init(&note, s, ELF_CORE_VENDOR, NT_PRFPREG, sizeof(*fpr));

    fpr = &note.n_un.un_prfpregset;
    for (i = 0; i < ARRAY_SIZE(env->fpregs); i++) {
        fpr->fpr_regs[i] = cpu_to_dump64(s, env->fpregs[i]);
    }
    fpr->fpr_cr = cpu_to_dump64(s, alpha_cpu_get_fpcr(env));

    ret = (*f)(&note, ELF_PRFPREG_SIZE, s);
    if (ret < 0) {
        return -1;
    }
    return 0;
}

static int alpha_cpu_write_elf64_prstatus(WriteCoreDumpFunction f,
                                          CPUAlphaState *env, int cpuid,
                                          DumpState *s)
{
    alpha_elf_prstatus_t *status;
    alpha_elf_note_t note;
    int ret, i;

    alpha_note_init(&note, s, ELF_CORE_VENDOR, NT_PRSTATUS, sizeof(*status));

    status = &note.n_un.un_prstatus;
    status->pr_version = ELF_PRSTATUS_VERSION;
    status->pr_statussz = sizeof(alpha_elf_prstatus_t);
    status->pr_gregsetsz = sizeof(alpha_elf_gregset_t);
    status->pr_fpregsetsz = sizeof(alpha_elf_fpregset_t);
    status->pr_osreldate = ELF_OSRELDATE;
    status->pr_cursig = 0;
    status->pr_pid = cpuid;
    for (i = 0; i < ARRAY_SIZE(env->gpregs); i++) {
        status->pr_reg.r_regs[i] =
                cpu_to_dump64(s, alpha_cpu_get_reg_value(env, i));
    }

    ret = (*f)(&note, ELF_PRSTATUS_SIZE, s);
    if (ret < 0) {
        return -1;
    }
    return 0;
}

int alpha_cpu_write_elf64_note(WriteCoreDumpFunction f, CPUState *cs,
                               int cpuid, DumpState *s)
{
    AlphaCPU *cpu = ALPHA_CPU(cs);
    CPUAlphaState *env = &cpu->env;
    int ret;

    ret = alpha_cpu_write_elf64_prstatus(f, env, cpuid, s);
    if (ret < 0) {
        return -1;
    }

    ret = alpha_write_elf64_prfpreg(f, env, cpuid, s);
    if (ret < 0) {
        return -1;
    }

    return 0;
}

int alpha_cpu_write_elf32_note(WriteCoreDumpFunction f, CPUState *cs,
                               int cpuid, DumpState *s)
{
    return -1;
}

void alpha_cpu_get_dump_info(const CPUState *cs, ArchDumpInfo *info,
                             const GuestPhysBlockList *guest_phys_blocks)
{
    hwaddr lowest_addr = ULLONG_MAX;
    GuestPhysBlock *block;

    /* Take a best guess at the physical address base. */
    QTAILQ_FOREACH(block, &guest_phys_blocks->head, next) {
        lowest_addr = MIN(lowest_addr, block->target_start);
    }

    info->d_machine = ELF_ARCH;
    info->d_endian = ELF_DATA;
    info->d_class = ELF_CLASS;
    info->page_size = TARGET_PAGE_SIZE;
    if (lowest_addr != ULLONG_MAX) {
        info->phys_base = lowest_addr;
    }
}

size_t alpha_cpu_get_note_size(const CPUState *cs, int class, int machine,
                               int nr_cpus)
{
    return (ELF_PRSTATUS_SIZE + ELF_PRFPREG_SIZE) * nr_cpus;
}
