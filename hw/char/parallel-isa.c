/*
 * QEMU Parallel PORT (ISA bus helpers)
 *
 * These functions reside in a separate file since they also might be
 * required for linking when compiling QEMU without CONFIG_PARALLEL.
 *
 * Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "system/system.h"
#include "hw/isa/isa.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/parallel-isa.h"
#include "hw/char/parallel.h"
#include "qapi/error.h"

static void parallel_init(ISABus *bus, int index, Chardev *chr)
{
    DeviceState *dev;
    ISADevice *isadev;

    isadev = isa_new(TYPE_ISA_PARALLEL);
    dev = DEVICE(isadev);
    qdev_prop_set_uint32(dev, "index", index);
    qdev_prop_set_chr(dev, "chardev", chr);
    isa_realize_and_unref(isadev, bus, &error_fatal);
}

void parallel_hds_isa_init(ISABus *bus, int n)
{
    int i;

    assert(n <= MAX_PARALLEL_PORTS);

    for (i = 0; i < n; i++) {
        if (parallel_hds[i]) {
            parallel_init(bus, i, parallel_hds[i]);
        }
    }
}

void isa_parallel_set_iobase(ISADevice *parallel, hwaddr iobase)
{
    ISAParallelState *s = ISA_PARALLEL(parallel);

    parallel->ioport_id = iobase;
    s->iobase = iobase;
    s->state.base = iobase;
    portio_list_set_address(&s->portio_list, s->iobase);
    portio_list_set_address(&s->portio_list_ext, s->iobase);
    isa_parallel_set_enabled(parallel, s->enabled);
}

void isa_parallel_set_enabled(ISADevice *parallel, bool enabled)
{
    ISAParallelState *s = ISA_PARALLEL(parallel);

    s->enabled = enabled;
    portio_list_set_enabled(&s->portio_list, enabled);
    portio_list_set_enabled(&s->portio_list_ext,
                            enabled && s->iobase != 0x3bc);
}
