/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Jensen's EISA Adaptec AHA-1742A, enhanced-mode subset.
 * Register/ECB layout: Adaptec 510208-00A, chapters 4 and 6.
 * The board's floppy function is disabled (EISA ID ADP0001).
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/isa/isa.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/scsi/scsi.h"
#include "scsi/constants.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_JENSEN_AHA1742 "jensen-aha1742"
OBJECT_DECLARE_SIMPLE_TYPE(JensenAha1742, JENSEN_AHA1742)

#define AHA_QUEUE_SIZE 64
#define AHA_MAX_SG 128
#define AHA_COMMAND_DELAY_NS (100 * 1000)
#define AHA_DI 0x0080
#define AHA_SES 0x0400
#define AHA_SG 0x1000
#define AHA_DSB 0x4000
#define AHA_ARS 0x8000

typedef struct AhaCompletion {
    uint32_t mailbox;
    uint8_t status;
} AhaCompletion;

typedef struct AhaRequest {
    JensenAha1742 *s;
    SCSIRequest *req;
    QEMUTimer *start_timer;
    QTAILQ_ENTRY(AhaRequest) next;
    uint32_t ecb, statusptr, senseptr, transferred;
    uint16_t flags;
    uint8_t target, senselen;
    struct {
        uint32_t addr, len;
    } sg[AHA_MAX_SG];
    unsigned nsg, index;
    uint32_t offset;
    uint64_t total;
    bool dma_error, resetting;
} AhaRequest;

struct JensenAha1742 {
    ISADevice parent_obj;
    SCSIBus bus;
    MemoryRegion io;
    qemu_irq irq;
    uint32_t slot, irq_num;
    uint8_t regs[0x80];
    uint32_t mailbox_out;
    bool out_full, hard_reset;
    AhaCompletion done[AHA_QUEUE_SIZE];
    unsigned head, count, active;
    QTAILQ_HEAD(, AhaRequest) requests;
};

static void aha_update_irq(JensenAha1742 *s)
{
    qemu_set_irq(s->irq, s->count && (s->regs[0x42] & 0x10));
}

static void aha_post(JensenAha1742 *s, uint32_t mailbox, uint8_t status)
{
    unsigned tail = (s->head + s->count) % AHA_QUEUE_SIZE;

    assert(s->count < AHA_QUEUE_SIZE);
    s->done[tail].mailbox = mailbox;
    s->done[tail].status = status;
    s->count++;
    aha_update_irq(s);
}

static void aha_finish(AhaRequest *r, uint8_t host, uint8_t target,
                       uint32_t residual, SCSIRequest *req)
{
    JensenAha1742 *s = r->s;
    uint8_t status[32] = { 0 };
    uint16_t flags = target ? 0x0100 : 1;
    uint8_t sense[SCSI_SENSE_BUF_SIZE];
    int len = 0;

    if (host) {
        flags = 0x0100 | ((host == 0x0a || host == 0x16 ||
                          host == 0x18 || host == 0x19 || host == 0x1a) ?
                         0x0010 : 0x1000);
    }
    if (residual) {
        flags |= 2;
    }
    if (req && target == CHECK_CONDITION && (r->flags & AHA_ARS)) {
        len = MIN(scsi_req_get_sense(req, sense, sizeof(sense)), r->senselen);
        if (len && dma_memory_write(&address_space_memory, r->senseptr,
                                    sense, len, MEMTXATTRS_UNSPECIFIED)) {
            host = 0x20;
            flags |= 0x1100;
        } else if (len) {
            flags |= 0x0300;
        }
    }
    if (host || target || !(r->flags & AHA_DI)) {
        flags |= 0x0080;
    }
    stw_le_p(status, flags);
    status[2] = host;
    status[3] = target;
    stl_le_p(status + 4, residual);
    status[12] = len ? 1 : 0;
    status[14] = len;
    /* DI/DSB suppress successful completions only (manual 6-5/6-6). */
    if ((host || target || !(r->flags & AHA_DSB)) &&
        dma_memory_write(&address_space_memory, r->statusptr, status,
                         sizeof(status), MEMTXATTRS_UNSPECIFIED)) {
        host = 0x20;
    }
    trace_jensen_aha_complete(r->ecb, host, target, residual);
    if (host || target || !(r->flags & AHA_DI)) {
        aha_post(s, r->ecb, (host || target ? 0xc0 : 0x10) | r->target);
    }
}

static void aha_complete(SCSIRequest *req, size_t residual)
{
    AhaRequest *r = req->hba_private;
    uint64_t left = r->total - MIN(r->total, r->transferred);
    uint8_t host = r->dma_error ? 0x20 : 0;

    if (req->host_status) {
        host = 0x20;
    }
    if (left && !(r->flags & AHA_SES) && !host &&
        req->status == GOOD) {
        host = 0x12;
    }
    aha_finish(r, host, req->status, MIN(left, UINT32_MAX), req);
    QTAILQ_REMOVE(&r->s->requests, r, next);
    r->s->active--;
    scsi_req_unref(req);
}

static void aha_cancel(SCSIRequest *req)
{
    AhaRequest *r = req->hba_private;

    if (!r->resetting) {
        aha_finish(r, 0x04, 0, 0, NULL);
    }
    QTAILQ_REMOVE(&r->s->requests, r, next);
    r->s->active--;
    scsi_req_unref(req);
}

static void aha_cancel_request(AhaRequest *r)
{
    if (r->start_timer) {
        timer_free(r->start_timer);
        r->start_timer = NULL;
        aha_cancel(r->req);
    } else {
        scsi_req_cancel(r->req);
    }
}

static void aha_execute(void *opaque)
{
    AhaRequest *r = opaque;
    SCSIRequest *req = r->req;
    int32_t data;

    timer_free(r->start_timer);
    r->start_timer = NULL;
    /* Enqueue can complete synchronously; keep req alive across that call. */
    scsi_req_ref(req);
    data = scsi_req_enqueue(req);
    if (data) {
        scsi_req_continue(req);
    }
    scsi_req_unref(req);
}

static void aha_free_request(SCSIBus *bus, void *priv)
{
    g_free(priv);
}

static void aha_transfer(SCSIRequest *req, uint32_t len)
{
    AhaRequest *r = req->hba_private;
    uint8_t *buf = scsi_req_get_buf(req);
    bool from_device = req->cmd.mode == SCSI_XFER_FROM_DEV;
    uint32_t done = 0;

    while (done < len && r->index < r->nsg) {
        uint32_t n = MIN(len - done, r->sg[r->index].len - r->offset);
        hwaddr addr = (uint64_t)r->sg[r->index].addr + r->offset;
        MemTxResult result;

        if (from_device) {
            result = dma_memory_write(&address_space_memory, addr, buf + done,
                                      n, MEMTXATTRS_UNSPECIFIED);
        } else {
            result = dma_memory_read(&address_space_memory, addr, buf + done,
                                     n, MEMTXATTRS_UNSPECIFIED);
        }
        r->dma_error |= result != MEMTX_OK;
        done += n;
        r->transferred += n;
        r->offset += n;
        if (r->offset == r->sg[r->index].len) {
            r->index++;
            r->offset = 0;
        }
    }
    if (done < len) {
        r->dma_error = true;
        if (!from_device) {
            memset(buf + done, 0, len - done);
        }
    }
    scsi_req_continue(req);
}

static const SCSIBusInfo aha_scsi_info = {
    .tcq = true,
    .max_target = 7,
    .max_lun = 7,
    .transfer_data = aha_transfer,
    .complete = aha_complete,
    .cancel = aha_cancel,
    .free_request = aha_free_request,
};

static void aha_start(JensenAha1742 *s, uint8_t target)
{
    uint8_t ecb[48], descriptors[AHA_MAX_SG * 8];
    uint32_t addr, length;
    AhaRequest *r = g_new0(AhaRequest, 1);
    SCSIDevice *dev;
    SCSIRequest *req;
    int32_t data;
    uint8_t error = 0x18;

    r->s = s;
    r->ecb = s->mailbox_out;
    r->target = target;
    if (dma_memory_read(&address_space_memory, r->ecb, ecb, sizeof(ecb),
                        MEMTXATTRS_UNSPECIFIED)) {
        aha_post(s, r->ecb, 0xc0 | target);
        g_free(r);
        return;
    }
    r->flags = lduw_le_p(ecb + 2);
    r->statusptr = ldl_le_p(ecb + 16);
    r->senseptr = ldl_le_p(ecb + 28);
    r->senselen = ecb[32];
    addr = ldl_le_p(ecb + 8);
    length = ldl_le_p(ecb + 12);
    trace_jensen_aha_command(r->ecb, target, lduw_le_p(ecb), ecb[36], length);
    trace_jensen_aha_ecb(r->ecb, r->flags, lduw_le_p(ecb + 4), addr,
                        r->statusptr, ldq_le_p(ecb + 36));
    if (r->flags & 1) {
        /* Chaining needs its own queue semantics; do not silently accept it. */
        goto invalid;
    }
    if (lduw_le_p(ecb) == 0) {
        aha_finish(r, 0, 0, 0, NULL);
        g_free(r);
        return;
    }
    if (lduw_le_p(ecb) == 0x0a) {
        uint8_t inquiry[56] = { 0x63, 0, 2, 0, 51, 0, AHA_QUEUE_SIZE };

        memcpy(inquiry + 8, "Adaptec ", 8);
        memcpy(inquiry + 16, "AHA-1742", 8);
        memcpy(inquiry + 24, "QEMU    ", 8);
        memcpy(inquiry + 32, "0001", 4);
        data = MIN(length, sizeof(inquiry));
        if (dma_memory_write(&address_space_memory, addr, inquiry, data,
                             MEMTXATTRS_UNSPECIFIED)) {
            error = 0x20;
            goto invalid;
        }
        aha_finish(r, 0, 0, length - data, NULL);
        g_free(r);
        return;
    }
    if (lduw_le_p(ecb) != 1) {
        error = 0x16;
        goto invalid;
    }
    if (!ecb[33] || ecb[33] > 12 || (lduw_le_p(ecb + 4) & 0x4c00)) {
        goto invalid;
    }
    if (r->flags & AHA_SG) {
        error = 0x1a;
        if (length > sizeof(descriptors) || (length & 7) ||
            dma_memory_read(&address_space_memory, addr, descriptors, length,
                            MEMTXATTRS_UNSPECIFIED)) {
            goto invalid;
        }
        r->nsg = length / 8;
        for (unsigned i = 0; i < r->nsg; i++) {
            r->sg[i].addr = ldl_le_p(descriptors + i * 8);
            r->sg[i].len = ldl_le_p(descriptors + i * 8 + 4);
            r->total += r->sg[i].len;
        }
    } else {
        r->nsg = 1;
        r->sg[0].addr = addr;
        r->sg[0].len = length;
        r->total = length;
    }
    dev = scsi_device_find(&s->bus, 0, target, lduw_le_p(ecb + 4) & 7);
    if (!dev) {
        aha_finish(r, 0x11, 0, 0, NULL);
        g_free(r);
        return;
    }
    req = scsi_req_new(dev, r->ecb, lduw_le_p(ecb + 4) & 7, ecb + 36,
                       ecb[33], r);
    if ((lduw_le_p(ecb + 4) & 0x0100) &&
        req->cmd.mode != SCSI_XFER_NONE &&
        !!(lduw_le_p(ecb + 4) & 0x0200) !=
        (req->cmd.mode == SCSI_XFER_FROM_DEV)) {
        aha_finish(r, 0x18, 0, 0, NULL);
        scsi_req_unref(req);
        return;
    }
    r->req = req;
    QTAILQ_INSERT_TAIL(&s->requests, r, next);
    s->active++;
    /*
     * Attention submits an ECB to the adapter; it does not execute the SCSI
     * command inside the host's register write. Keep even a cache hit or
     * TEST UNIT READY asynchronous, as on the bus-mastering adapter.
     */
    r->start_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, aha_execute, r);
    timer_mod(r->start_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + AHA_COMMAND_DELAY_NS);
    return;

invalid:
    aha_finish(r, error, 0, 0, NULL);
    g_free(r);
}

static void aha_reset_adapter(JensenAha1742 *s)
{

    while (!QTAILQ_EMPTY(&s->requests)) {
        AhaRequest *r = QTAILQ_FIRST(&s->requests);

        r->resetting = true;
        aha_cancel_request(r);
    }
    qemu_set_irq(s->irq, 0);
    /* G2CNTRL.HRST resets the adapter, not the EISA configuration. */
    memset(s->regs + 0x50, 0, sizeof(s->regs) - 0x50);
    s->mailbox_out = 0;
    s->out_full = s->hard_reset = false;
    s->head = s->count = 0;
}

static void aha_reset(DeviceState *dev)
{
    JensenAha1742 *s = JENSEN_AHA1742(dev);
    static const uint8_t irq_code[] = { 9, 10, 11, 12, 0, 14, 15, 0 };
    unsigned code;

    aha_reset_adapter(s);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[4] = 1;
    s->regs[0x40] = 0x80;
    for (code = 0; code < ARRAY_SIZE(irq_code); code++) {
        if (irq_code[code] == s->irq_num) {
            break;
        }
    }
    s->regs[0x42] = code | 0x10;
    s->regs[0x43] = 7;
    s->irq = isa_get_irq(ISA_DEVICE(s), s->irq_num);
}

static uint8_t aha_read_byte(JensenAha1742 *s, unsigned reg)
{
    static const uint8_t id[] = { 0x04, 0x90, 0x00, 0x01 };

    if (reg >= sizeof(s->regs)) {
        return 0xff;
    }
    if (reg < sizeof(id)) {
        return id[reg];
    }
    if (reg >= 0x50 && reg <= 0x53) {
        return s->mailbox_out >> ((reg - 0x50) * 8);
    }
    if (reg == 0x56) {
        return s->count ? s->done[s->head].status : 0;
    }
    if (reg == 0x57) {
        return (s->hard_reset ? 1 : 0) | (s->count ? 2 : 0) |
               (!s->out_full && s->count + s->active < AHA_QUEUE_SIZE ? 4 : 0);
    }
    if (reg >= 0x58 && reg <= 0x5b) {
        return s->count ? s->done[s->head].mailbox >> ((reg - 0x58) * 8) : 0;
    }
    if (reg == 0x5c) {
        return 0; /* Immediate reset consumes host-ready synchronously. */
    }
    return s->regs[reg];
}

static void aha_write_byte(JensenAha1742 *s, unsigned reg, uint8_t value)
{
    static const uint8_t irq_code[] = { 9, 10, 11, 12, 0, 14, 15, 0 };

    if (reg < 4 || reg >= sizeof(s->regs)) {
        return;
    }
    if (reg >= 0x50 && reg <= 0x53) {
        unsigned shift = (reg - 0x50) * 8;

        s->mailbox_out = (s->mailbox_out & ~(0xffU << shift)) |
                         (uint32_t)value << shift;
        s->out_full = true;
    } else if (reg == 0x54) {
        s->regs[reg] = value;
        s->out_full = false;
        if (s->count + s->active >= AHA_QUEUE_SIZE || s->hard_reset) {
            return;
        }
        switch (value & 0xf0) {
        case 0x40:
            aha_start(s, value & 7);
            break;
        case 0x10:
            if ((s->mailbox_out & 0xff) == 0x80) {
                scsi_bus_set_ua(&s->bus, SENSE_CODE(RESET));
                aha_post(s, s->mailbox_out, 0xa0 | (value & 7));
            } else {
                aha_post(s, s->mailbox_out, 0xe0 | (value & 7));
            }
            break;
        case 0x50: {
            AhaRequest *r;

            QTAILQ_FOREACH(r, &s->requests, next) {
                if (r->ecb == s->mailbox_out) {
                    aha_cancel_request(r);
                    break;
                }
            }
            break;
        }
        }
    } else if (reg == 0x55) {
        if (value & 0x80) {
            aha_reset_adapter(s);
            s->hard_reset = true;
        } else {
            s->hard_reset = false;
            if ((value & 0x40) && s->count) {
                qemu_set_irq(s->irq, 0);
                s->head = (s->head + 1) % AHA_QUEUE_SIZE;
                s->count--;
                aha_update_irq(s);
            }
        }
    } else if (reg == 0x42) {
        qemu_set_irq(s->irq, 0);
        s->regs[reg] = value;
        s->irq = irq_code[value & 7] ?
                 isa_get_irq(ISA_DEVICE(s), irq_code[value & 7]) : NULL;
        aha_update_irq(s);
    } else if (reg < 0x56 || reg > 0x5c) {
        s->regs[reg] = value;
    }
}

static uint64_t aha_read(void *opaque, hwaddr addr, unsigned size)
{
    JensenAha1742 *s = opaque;
    uint64_t value = 0;

    for (unsigned i = 0; i < size; i++) {
        value |= (uint64_t)aha_read_byte(s, addr + i) << (i * 8);
    }
    trace_jensen_aha_read(addr, value, size);
    return value;
}

static void aha_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    JensenAha1742 *s = opaque;

    trace_jensen_aha_write(addr, value, size);
    for (unsigned i = 0; i < size; i++) {
        aha_write_byte(s, addr + i, value >> (i * 8));
    }
}

static const MemoryRegionOps aha_io_ops = {
    .read = aha_read,
    .write = aha_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

static void aha_realize(DeviceState *dev, Error **errp)
{
    JensenAha1742 *s = JENSEN_AHA1742(dev);
    ISADevice *isa = ISA_DEVICE(dev);

    if (s->slot < 1 || s->slot > 6 ||
        (s->irq_num != 9 && s->irq_num != 10 && s->irq_num != 11 &&
         s->irq_num != 12 && s->irq_num != 14 && s->irq_num != 15)) {
        error_setg(errp, "AHA-1742 requires EISA slot 1..6 and a valid IRQ");
        return;
    }
    QTAILQ_INIT(&s->requests);
    scsi_bus_init(&s->bus, sizeof(s->bus), dev, &aha_scsi_info);
    memory_region_init_io(&s->io, OBJECT(s), &aha_io_ops, s,
                          "aha1742-eisa", 0x80);
    isa_register_ioport(isa, &s->io, (s->slot << 12) | 0xc80);
    aha_reset(dev);
}

static const Property aha_properties[] = {
    DEFINE_PROP_UINT32("slot", JensenAha1742, slot, 6),
    DEFINE_PROP_UINT32("irq", JensenAha1742, irq_num, 11),
};

static const VMStateDescription aha_vmstate = {
    .name = TYPE_JENSEN_AHA1742,
    .unmigratable = true,
};

static void aha_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = aha_realize;
    dc->vmsd = &aha_vmstate;
    device_class_set_legacy_reset(dc, aha_reset);
    device_class_set_props(dc, aha_properties);
    dc->desc = "Jensen EISA AHA-1742A enhanced-mode subset";
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo aha_type_info = {
    .name = TYPE_JENSEN_AHA1742,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(JensenAha1742),
    .class_init = aha_class_init,
};

static void aha_register_types(void)
{
    type_register_static(&aha_type_info);
}

type_init(aha_register_types)

