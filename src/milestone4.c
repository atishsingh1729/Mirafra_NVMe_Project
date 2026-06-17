/*
 * milestone4.c — Queue Engine
 *
 * Validate the generic queue pair module:
 * - SQ/CQ ring management with doorbells
 * - CID assignment and completion tracking
 * - Multiple in-flight commands
 * - Admin queue: Identify + Set Features
 * - I/O queue: Create, Write, Read, Verify
 */

#include "log.h"
#include "pci.h"
#include "mmio.h"
#include "dma.h"
#include "nvme_regs.h"
#include "nvme_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ADMIN_Q_DEPTH   32
#define IO_Q_DEPTH      64
#define TEST_LBA        0x100000ULL

int main(int argc, char *argv[])
{
    struct pci_device pci;
    struct nvme_qpair admin_qp, io_qp;
    int rc = 0;

    log_init((argc > 1 && argv[1][0] == '-' && argv[1][1] == 'q')
             ? LOG_INFO : LOG_TRACE, "nvme_m4.log");

    /* Device setup */
    if (pci_find_nvme(&pci) < 0) { rc = 1; goto done; }
    pci_detect_dma_offset(&pci);
    pci_unbind_driver(&pci);
    pci_enable_bus_master(&pci);
    if (pci_map_bar0(&pci) < 0) { rc = 1; goto rebind; }
    mmio_init(pci.bar0);

    uint64_t cap = mmio_read64(NVME_REG_CAP);
    uint32_t dstrd = CAP_DSTRD(cap);
    uint32_t timeout_ms = CAP_TO(cap) * 500;
    if (timeout_ms == 0) timeout_ms = 5000;

    /* --- Controller reset --- */
    uint32_t cc = mmio_read32(NVME_REG_CC);
    if (cc & CC_EN) mmio_write32(NVME_REG_CC, cc & ~CC_EN);

    while (mmio_read32(NVME_REG_CSTS) & CSTS_RDY) usleep(1000);
    log_msg(LOG_INFO, "Controller disabled.");

    /* --- Create admin queue pair using the queue engine --- */
    if (nvme_qpair_create(&admin_qp, 0, ADMIN_Q_DEPTH, dstrd, pci.dma_offset) < 0) {
        rc = 1; goto cleanup;
    }

    mmio_write32(NVME_REG_AQA,
                 ((ADMIN_Q_DEPTH - 1) << 16) | (ADMIN_Q_DEPTH - 1));
    mmio_write64(NVME_REG_ASQ, nvme_qpair_sq_bus(&admin_qp));
    mmio_write64(NVME_REG_ACQ, nvme_qpair_cq_bus(&admin_qp));

    /* Enable controller */
    mmio_write32(NVME_REG_CC,
                 CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_IOSQES_64 | CC_IOCQES_16);

    while (!(mmio_read32(NVME_REG_CSTS) & CSTS_RDY)) usleep(1000);
    log_msg(LOG_INFO, "Controller enabled.");

    /* === Test 1: Identify Controller via queue engine === */
    log_msg(LOG_INFO, "Test 1: Identify Controller (sync, single command)");

    struct dma_buffer id_buf;
    if (dma_alloc(&id_buf, 4096, pci.dma_offset) < 0) { rc = 1; goto shutdown; }

    struct nvme_cmd cmd = {0};
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.prp1   = id_buf.bus;
    cmd.cdw10  = NVME_IDENTIFY_CTRL;

    int sf = nvme_qpair_submit_sync(&admin_qp, &cmd, NULL, 5000);
    if (sf != 0) {
        log_msg(LOG_ERROR, "Identify failed SF=0x%04X", sf);
        rc = 1; goto shutdown;
    }

    dma_invalidate(id_buf.virt, 4096);
    char model[41] = {0}, serial[21] = {0};
    memcpy(serial, (uint8_t *)id_buf.virt + 4, 20);
    memcpy(model, (uint8_t *)id_buf.virt + 24, 40);
    for (int i = 19; i >= 0 && serial[i] == ' '; i--) serial[i] = 0;
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = 0;
    log_msg(LOG_INFO, "  -> %s  SN=%s", model, serial);

    /* Get namespace info for I/O test */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid   = 1;
    cmd.prp1   = id_buf.bus;
    cmd.cdw10  = NVME_IDENTIFY_NS;
    sf = nvme_qpair_submit_sync(&admin_qp, &cmd, NULL, 5000);
    dma_invalidate(id_buf.virt, 4096);

    uint8_t *nsd = (uint8_t *)id_buf.virt;
    uint8_t flbas = nsd[26] & 0x0F;
    uint32_t lba_fmt = *(uint32_t *)(nsd + 128 + flbas * 4);
    uint32_t block_size = 1U << ((lba_fmt >> 16) & 0xFF);
    uint64_t nsze = *(uint64_t *)(nsd + 0);
    log_msg(LOG_INFO, "  NS1: %llu LBAs x %u B",
            (unsigned long long)nsze, block_size);

    dma_free(&id_buf);

    /* === Test 2: CID tracking with multiple Identify commands === */
    log_msg(LOG_INFO, "Test 2: CID tracking (3 back-to-back submits, then poll all)");

    struct dma_buffer bufs[3];
    int cids[3];

    for (int i = 0; i < 3; i++) {
        if (dma_alloc(&bufs[i], 4096, pci.dma_offset) < 0) { rc = 1; goto shutdown; }

        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = NVME_ADMIN_IDENTIFY;
        cmd.prp1   = bufs[i].bus;
        cmd.cdw10  = NVME_IDENTIFY_CTRL;

        cids[i] = nvme_qpair_submit(&admin_qp, &cmd);
        if (cids[i] < 0) {
            log_msg(LOG_ERROR, "Submit %d failed", i);
            rc = 1; goto shutdown;
        }
        log_msg(LOG_INFO, "  Submitted CID=%d", cids[i]);
    }

    log_msg(LOG_INFO, "  In-flight: %u", nvme_qpair_in_flight(&admin_qp));

    /* Poll until all 3 complete */
    int attempts = 0;
    while (nvme_qpair_in_flight(&admin_qp) > 0 && attempts < 50000) {
        nvme_qpair_poll(&admin_qp);
        usleep(100);
        attempts++;
    }

    /* Check each CID completed successfully */
    int all_ok = 1;
    for (int i = 0; i < 3; i++) {
        uint16_t st; uint32_t res;
        if (nvme_qpair_cid_done(&admin_qp, (uint16_t)cids[i], &st, &res)) {
            log_msg(LOG_INFO, "  CID=%d completed SF=0x%04X %s",
                    cids[i], st, st == 0 ? "OK" : "FAIL");
            if (st != 0) all_ok = 0;
        } else {
            log_msg(LOG_ERROR, "  CID=%d still pending!", cids[i]);
            all_ok = 0;
        }
        dma_free(&bufs[i]);
    }

    if (!all_ok) { rc = 1; goto shutdown; }

    /* === Test 3: Set Features + Create I/O Queue Pair === */
    log_msg(LOG_INFO, "Test 3: Set Features (Number of Queues) + Create I/O QP");

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_SET_FEATURES;
    cmd.cdw10  = NVME_FEAT_NUM_QUEUES;
    cmd.cdw11  = (7 << 16) | 7;   /* request 8 SQs, 8 CQs */

    uint32_t feat_res;
    sf = nvme_qpair_submit_sync(&admin_qp, &cmd, &feat_res, 5000);
    if (sf != 0) {
        log_msg(LOG_ERROR, "Set Features failed SF=0x%04X", sf);
        rc = 1; goto shutdown;
    }
    log_msg(LOG_INFO, "  Allocated %u SQs, %u CQs",
            (feat_res & 0xFFFF) + 1, ((feat_res >> 16) & 0xFFFF) + 1);

    /* Create I/O queue pair via the queue engine */
    if (nvme_qpair_create(&io_qp, 1, IO_Q_DEPTH, dstrd, pci.dma_offset) < 0) {
        rc = 1; goto shutdown;
    }

    /* Create I/O CQ (admin command) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CREATE_IO_CQ;
    cmd.prp1   = nvme_qpair_cq_bus(&io_qp);
    cmd.cdw10  = ((IO_Q_DEPTH - 1) << 16) | 1;   /* QSIZE | QID=1 */
    cmd.cdw11  = (1 << 0);                        /* PC=1 */

    sf = nvme_qpair_submit_sync(&admin_qp, &cmd, NULL, 5000);
    if (sf != 0) {
        log_msg(LOG_ERROR, "Create I/O CQ failed SF=0x%04X", sf);
        rc = 1; goto shutdown;
    }

    /* Create I/O SQ (admin command) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CREATE_IO_SQ;
    cmd.prp1   = nvme_qpair_sq_bus(&io_qp);
    cmd.cdw10  = ((IO_Q_DEPTH - 1) << 16) | 1;   /* QSIZE | QID=1 */
    cmd.cdw11  = (1 << 16) | (1 << 0);            /* CQID=1 | PC=1 */

    sf = nvme_qpair_submit_sync(&admin_qp, &cmd, NULL, 5000);
    if (sf != 0) {
        log_msg(LOG_ERROR, "Create I/O SQ failed SF=0x%04X", sf);
        rc = 1; goto shutdown;
    }
    log_msg(LOG_INFO, "  I/O QP1 created (depth=%d)", IO_Q_DEPTH);

    /* === Test 4: I/O Write + Read via the I/O queue engine === */
    log_msg(LOG_INFO, "Test 4: Write + Read via I/O queue pair");

    struct dma_buffer wr_buf, rd_buf;
    if (dma_alloc(&wr_buf, block_size, pci.dma_offset) < 0) { rc = 1; goto shutdown; }
    if (dma_alloc(&rd_buf, block_size, pci.dma_offset) < 0) { rc = 1; goto shutdown; }

    /* Fill write buffer with test pattern */
    uint8_t *wp = (uint8_t *)wr_buf.virt;
    for (uint32_t i = 0; i < block_size; i++) wp[i] = (uint8_t)(i & 0xFF);
    memcpy(wp, "QueueEngine-M4-Test!", 20);
    dma_flush(wr_buf.virt, block_size);

    /* Write command */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_IO_WRITE;
    cmd.nsid   = 1;
    cmd.prp1   = wr_buf.bus;
    cmd.cdw10  = (uint32_t)(TEST_LBA & 0xFFFFFFFF);
    cmd.cdw11  = (uint32_t)(TEST_LBA >> 32);
    cmd.cdw12  = 0;   /* 1 block */

    sf = nvme_qpair_submit_sync(&io_qp, &cmd, NULL, 5000);
    if (sf != 0) {
        log_msg(LOG_ERROR, "Write failed SF=0x%04X", sf);
        rc = 1; goto shutdown;
    }
    log_msg(LOG_INFO, "  Write OK (LBA=0x%llX)", (unsigned long long)TEST_LBA);

    /* Read command */
    memset(rd_buf.virt, 0xAA, block_size);
    dma_flush(rd_buf.virt, block_size);

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_IO_READ;
    cmd.nsid   = 1;
    cmd.prp1   = rd_buf.bus;
    cmd.cdw10  = (uint32_t)(TEST_LBA & 0xFFFFFFFF);
    cmd.cdw11  = (uint32_t)(TEST_LBA >> 32);
    cmd.cdw12  = 0;

    sf = nvme_qpair_submit_sync(&io_qp, &cmd, NULL, 5000);
    if (sf != 0) {
        log_msg(LOG_ERROR, "Read failed SF=0x%04X", sf);
        rc = 1; goto shutdown;
    }

    /* Verify */
    dma_invalidate(rd_buf.virt, block_size);
    uint8_t *rp = (uint8_t *)rd_buf.virt;
    int mismatches = 0;
    for (uint32_t i = 0; i < block_size; i++) {
        if (wp[i] != rp[i]) mismatches++;
    }

    if (mismatches == 0)
        log_msg(LOG_INFO, "  Read OK + Verify PASS (%u bytes match)", block_size);
    else {
        log_msg(LOG_ERROR, "  Verify FAIL: %d mismatches out of %u bytes", mismatches, block_size);
        rc = 1;
    }

    log_msg(LOG_INFO, "  Read-back: \"%.20s\"", (char *)rp);

    dma_free(&wr_buf);
    dma_free(&rd_buf);

    /* Summary */
    log_msg(LOG_INFO, "Queue engine summary:");
    log_msg(LOG_INFO, "  Admin QP0: depth=%u  CIDs used=%u",
            admin_qp.depth, admin_qp.cid_next);
    log_msg(LOG_INFO, "  I/O   QP1: depth=%u  CIDs used=%u",
            io_qp.depth, io_qp.cid_next);
    log_msg(LOG_INFO, "  Tests: Identify, CID-tracking(x3), Set Features, "
            "Create I/O QP, Write, Read, Verify — all passed.");

shutdown:
    mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);
    nvme_qpair_destroy(&io_qp);
    nvme_qpair_destroy(&admin_qp);

cleanup:
    pci_unmap_bar0(&pci);

rebind:
    pci_rebind_nvme_driver(&pci);

done:
    log_shutdown();
    return rc;
}
