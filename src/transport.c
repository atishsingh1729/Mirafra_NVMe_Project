#include "transport.h"
#include "mmio.h"
#include "nvme_regs.h"
#include "log.h"
#include <string.h>
#include <unistd.h>
#include <time.h>

/* ── Default hooks ─────────────────────────────────────────── */

/* Pin buffer: allocate DMA, copy user data (for writes), flush */
static int default_pre_submit(struct transport_req *req, void *ctx) {
    struct transport *t = (struct transport *)ctx;
    if (!req->buf || req->buf_len == 0) return 0;

    if (dma_alloc(&req->dma, req->buf_len, t->pci->dma_offset) < 0) {
        log_msg(LOG_ERROR, "transport: DMA alloc failed (%zu bytes)", req->buf_len);
        return -1;
    }

    /* For writes: copy user data into DMA buffer and flush to RAM */
    if (req->opcode == NVME_IO_WRITE) {
        memcpy(req->dma.virt, req->buf, req->buf_len);
        dma_flush(req->dma.virt, req->buf_len);
    } else {
        /* For reads/identify: just flush the zeroed buffer */
        dma_flush(req->dma.virt, req->buf_len);
    }

    log_msg(LOG_DEBUG, "transport: pre_submit opcode=0x%02X dma_bus=0x%llX len=%zu",
            req->opcode, (unsigned long long)req->dma.bus, req->buf_len);
    return 0;
}

/* Read CQE status, invalidate cache, copy data back (for reads) */
static void default_post_complete(struct transport_req *req, void *ctx) {
    (void)ctx;
    req->completed = 1;

    if (req->status != 0) {
        log_msg(LOG_DEBUG, "transport: post_complete opcode=0x%02X SF=0x%04X (error)",
                req->opcode, req->status);
        goto cleanup;
    }

    /* For reads/identify: invalidate cache then copy to user buffer */
    if (req->buf && req->buf_len > 0 && req->dma.virt &&
        req->opcode != NVME_IO_WRITE) {
        dma_invalidate(req->dma.virt, req->buf_len);
        memcpy(req->buf, req->dma.virt, req->buf_len);
    }

    log_msg(LOG_DEBUG, "transport: post_complete opcode=0x%02X SF=0x%04X OK",
            req->opcode, req->status);

cleanup:
    if (req->dma.virt) dma_free(&req->dma);
}

/* Reset: disable, destroy queues, rebuild, re-enable */
static int default_reset(void *ctx) {
    struct transport *t = (struct transport *)ctx;

    log_msg(LOG_INFO, "transport: reset triggered");

    /* Disable controller */
    mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);

    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    while (mmio_read32(NVME_REG_CSTS) & CSTS_RDY) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t0.tv_sec)*1000 + (now.tv_nsec - t0.tv_nsec)/1000000;
        if (ms > (long)t->to_ms) return -1;
        usleep(1000);
    }

    /* Destroy old queues */
    if (t->io_ready) { nvme_qpair_destroy(&t->io); t->io_ready = 0; }
    nvme_qpair_destroy(&t->admin);

    /* Recreate admin queue */
    if (nvme_qpair_create(&t->admin, 0, 32, t->dstrd, t->pci->dma_offset) < 0)
        return -1;

    mmio_write32(NVME_REG_AQA, (31 << 16) | 31);
    mmio_write64(NVME_REG_ASQ, nvme_qpair_sq_bus(&t->admin));
    mmio_write64(NVME_REG_ACQ, nvme_qpair_cq_bus(&t->admin));

    /* Re-enable */
    mmio_write32(NVME_REG_CC, CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_IOSQES_64 | CC_IOCQES_16);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!(mmio_read32(NVME_REG_CSTS) & CSTS_RDY)) {
        uint32_t csts = mmio_read32(NVME_REG_CSTS);
        if ((csts & CSTS_CFS) || csts == 0xFFFFFFFF) return -1;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t0.tv_sec)*1000 + (now.tv_nsec - t0.tv_nsec)/1000000;
        if (ms > (long)t->to_ms) return -1;
        usleep(1000);
    }

    log_msg(LOG_INFO, "transport: reset complete, controller ready");
    return 0;
}

/* ── Public API ────────────────────────────────────────────── */

int transport_init(struct transport *t, struct pci_device *pci) {
    memset(t, 0, sizeof(*t));
    t->pci = pci;

    uint64_t cap = mmio_read64(NVME_REG_CAP);
    t->dstrd = CAP_DSTRD(cap);
    t->to_ms = CAP_TO(cap) * 500;
    if (t->to_ms == 0) t->to_ms = 5000;

    /* Install default hooks */
    t->pre_submit   = default_pre_submit;
    t->post_complete = default_post_complete;
    t->reset        = default_reset;
    t->hook_ctx     = t;

    /* Reset and enable controller */
    if (mmio_read32(NVME_REG_CC) & CC_EN)
        mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);
    while (mmio_read32(NVME_REG_CSTS) & CSTS_RDY) usleep(1000);

    if (nvme_qpair_create(&t->admin, 0, 32, t->dstrd, pci->dma_offset) < 0)
        return -1;

    mmio_write32(NVME_REG_AQA, (31 << 16) | 31);
    mmio_write64(NVME_REG_ASQ, nvme_qpair_sq_bus(&t->admin));
    mmio_write64(NVME_REG_ACQ, nvme_qpair_cq_bus(&t->admin));
    mmio_write32(NVME_REG_CC, CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_IOSQES_64 | CC_IOCQES_16);

    while (!(mmio_read32(NVME_REG_CSTS) & CSTS_RDY)) usleep(1000);

    /* Identify namespace 1 to get block size */
    struct transport_req id_req = {0};
    uint8_t ns_buf[4096];
    id_req.opcode = NVME_ADMIN_IDENTIFY;
    id_req.nsid = 1;
    id_req.cdw10 = NVME_IDENTIFY_NS;
    id_req.buf = ns_buf;
    id_req.buf_len = 4096;

    if (transport_submit(t, &id_req) != 0) {
        t->block_size = 512;  /* fallback */
    } else {
        uint8_t flbas = ns_buf[26] & 0x0F;
        uint32_t fmt;
        memcpy(&fmt, ns_buf + 128 + flbas * 4, 4);
        uint8_t lbads = (fmt >> 16) & 0xFF;
        t->block_size = lbads ? (1U << lbads) : 512;
    }

    log_msg(LOG_INFO, "transport: init OK, block_size=%u", t->block_size);
    return 0;
}

int transport_create_io_queue(struct transport *t, uint16_t depth) {
    /* Set Features: Number of Queues */
    struct transport_req sf_req = {0};
    sf_req.opcode = NVME_ADMIN_SET_FEATURES;
    sf_req.cdw10 = NVME_FEAT_NUM_QUEUES;
    sf_req.cdw11 = (7 << 16) | 7;
    transport_submit(t, &sf_req);

    /* Create the I/O queue pair */
    if (nvme_qpair_create(&t->io, 1, depth, t->dstrd, t->pci->dma_offset) < 0)
        return -1;

    /* Create I/O CQ via raw admin command (PRP1 = CQ bus address) */
    struct nvme_cmd raw = {0};
    raw.opcode = NVME_ADMIN_CREATE_IO_CQ;
    raw.prp1 = nvme_qpair_cq_bus(&t->io);
    raw.cdw10 = ((depth - 1) << 16) | 1;
    raw.cdw11 = 1;
    int sf = nvme_qpair_submit_sync(&t->admin, &raw, NULL, 5000);
    if (sf != 0) {
        log_msg(LOG_ERROR, "transport: Create I/O CQ failed SF=0x%04X", sf);
        nvme_qpair_destroy(&t->io);
        return sf;
    }

    /* Create I/O SQ */
    memset(&raw, 0, sizeof(raw));
    raw.opcode = NVME_ADMIN_CREATE_IO_SQ;
    raw.prp1 = nvme_qpair_sq_bus(&t->io);
    raw.cdw10 = ((depth - 1) << 16) | 1;
    raw.cdw11 = (1 << 16) | 1;  /* CQID=1 | PC=1 */
    sf = nvme_qpair_submit_sync(&t->admin, &raw, NULL, 5000);
    if (sf != 0) {
        log_msg(LOG_ERROR, "transport: Create I/O SQ failed SF=0x%04X", sf);
        nvme_qpair_destroy(&t->io);
        return sf;
    }

    t->io_ready = 1;
    log_msg(LOG_INFO, "transport: I/O queue created (depth=%u)", depth);
    return 0;
}

int transport_submit(struct transport *t, struct transport_req *req) {
    req->completed = 0;
    req->status = 0xFFFF;
    memset(&req->dma, 0, sizeof(req->dma));

    /* Pre-submit hook: DMA alloc, copy data, flush cache */
    if (t->pre_submit) {
        if (t->pre_submit(req, t->hook_ctx) < 0) return -1;
    }

    /* Build SQE */
    struct nvme_cmd cmd = {0};
    cmd.opcode = req->opcode;
    cmd.nsid   = req->nsid;
    cmd.prp1   = req->dma.virt ? req->dma.bus : 0;
    cmd.cdw10  = req->cdw10;
    cmd.cdw11  = req->cdw11;

    /* For I/O commands: fill LBA and block count */
    if (req->is_io) {
        cmd.cdw10 = (uint32_t)(req->lba & 0xFFFFFFFF);
        cmd.cdw11 = (uint32_t)(req->lba >> 32);
        cmd.cdw12 = req->num_blocks;
    }

    /* Pick the right queue */
    struct nvme_qpair *qp = &t->admin;
    if (req->is_io && t->io_ready)
        qp = &t->io;

    /* Submit and wait */
    uint32_t res = 0;
    int sf = nvme_qpair_submit_sync(qp, &cmd, &res, 5000);
    req->status = (sf < 0) ? 0xFFFF : (uint16_t)sf;
    req->result = res;

    /* Post-complete hook: invalidate cache, copy data back, set status */
    if (t->post_complete)
        t->post_complete(req, t->hook_ctx);

    return (int)req->status;
}

int transport_reset(struct transport *t) {
    if (t->reset)
        return t->reset(t->hook_ctx);
    return -1;
}

void transport_shutdown(struct transport *t) {
    if (mmio_read32(NVME_REG_CC) & CC_EN)
        mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);
    if (t->io_ready) { nvme_qpair_destroy(&t->io); t->io_ready = 0; }
    nvme_qpair_destroy(&t->admin);
}

void transport_set_hooks(struct transport *t, hook_pre_submit_fn pre,
                         hook_post_complete_fn post, hook_reset_fn rst, void *ctx) {
    if (pre)  t->pre_submit   = pre;
    if (post) t->post_complete = post;
    if (rst)  t->reset        = rst;
    if (ctx)  t->hook_ctx     = ctx;
}
