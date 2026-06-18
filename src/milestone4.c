/* milestone4.c — Queue Engine: ring mgmt, CID tracking, multi-command, I/O round-trip */
#include "log.h"
#include "pci.h"
#include "mmio.h"
#include "dma.h"
#include "nvme_regs.h"
#include "nvme_queue.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define ADM_DEPTH 32
#define IO_DEPTH  64
#define TEST_LBA  0x100000ULL

int main(int argc, char *argv[]) {
    struct pci_device pci; struct nvme_qpair admin, io;
    int rc = 0;
    log_init((argc > 1 && argv[1][0] == '-' && argv[1][1] == 'q') ? LOG_INFO : LOG_TRACE, "nvme_m4.log");

    if (pci_find_nvme(&pci) < 0) { rc=1; goto done; }
    pci_detect_dma_offset(&pci); pci_unbind_driver(&pci); pci_enable_bus_master(&pci);
    if (pci_map_bar0(&pci) < 0) { rc=1; goto rebind; }
    mmio_init(pci.bar0);

    uint64_t cap = mmio_read64(NVME_REG_CAP);
    uint32_t dstrd = CAP_DSTRD(cap);

    /* Reset controller */
    mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);
    while (mmio_read32(NVME_REG_CSTS) & CSTS_RDY) usleep(1000);

    /* Admin queue via queue engine */
    if (nvme_qpair_create(&admin, 0, ADM_DEPTH, dstrd, pci.dma_offset) < 0) { rc=1; goto cleanup; }
    mmio_write32(NVME_REG_AQA, ((ADM_DEPTH-1) << 16) | (ADM_DEPTH-1));
    mmio_write64(NVME_REG_ASQ, nvme_qpair_sq_bus(&admin));
    mmio_write64(NVME_REG_ACQ, nvme_qpair_cq_bus(&admin));
    mmio_write32(NVME_REG_CC, CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_IOSQES_64 | CC_IOCQES_16);
    while (!(mmio_read32(NVME_REG_CSTS) & CSTS_RDY)) usleep(1000);

    /* Test 1: single Identify */
    struct dma_buffer buf; struct nvme_cmd cmd = {0};
    dma_alloc(&buf, 4096, pci.dma_offset);
    cmd.opcode = NVME_ADMIN_IDENTIFY; cmd.prp1 = buf.bus; cmd.cdw10 = NVME_IDENTIFY_CTRL;
    int sf = nvme_qpair_submit_sync(&admin, &cmd, NULL, 5000);
    if (sf == 0) {
        dma_invalidate(buf.virt, 4096);
        char mn[41]={0}; memcpy(mn, (uint8_t*)buf.virt+24, 40);
        log_msg(LOG_INFO, "Identify: %s", mn);
    } else { log_msg(LOG_ERROR, "Identify failed"); rc=1; goto shutdown; }

    /* Test 2: 3 concurrent CIDs */
    struct dma_buffer b3[3]; int cids[3];
    for (int i = 0; i < 3; i++) {
        dma_alloc(&b3[i], 4096, pci.dma_offset);
        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = NVME_ADMIN_IDENTIFY; cmd.prp1 = b3[i].bus; cmd.cdw10 = NVME_IDENTIFY_CTRL;
        cids[i] = nvme_qpair_submit(&admin, &cmd);
    }
    log_msg(LOG_INFO, "Submitted 3 cmds, in-flight=%u", nvme_qpair_in_flight(&admin));
    for (int t = 0; nvme_qpair_in_flight(&admin) > 0 && t < 50000; t++) { nvme_qpair_poll(&admin); usleep(100); }
    for (int i = 0; i < 3; i++) {
        uint16_t st; nvme_qpair_cid_done(&admin, cids[i], &st, NULL);
        log_msg(LOG_INFO, "  CID=%d SF=0x%04X", cids[i], st);
        dma_free(&b3[i]);
    }

    /* Test 3: I/O queue pair */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_SET_FEATURES; cmd.cdw10 = NVME_FEAT_NUM_QUEUES; cmd.cdw11 = (7<<16)|7;
    nvme_qpair_submit_sync(&admin, &cmd, NULL, 5000);

    nvme_qpair_create(&io, 1, IO_DEPTH, dstrd, pci.dma_offset);
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CREATE_IO_CQ; cmd.prp1 = nvme_qpair_cq_bus(&io);
    cmd.cdw10 = ((IO_DEPTH-1)<<16)|1; cmd.cdw11 = 1;
    nvme_qpair_submit_sync(&admin, &cmd, NULL, 5000);
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CREATE_IO_SQ; cmd.prp1 = nvme_qpair_sq_bus(&io);
    cmd.cdw10 = ((IO_DEPTH-1)<<16)|1; cmd.cdw11 = (1<<16)|1;
    nvme_qpair_submit_sync(&admin, &cmd, NULL, 5000);

    /* Test 4: Write + Read + Verify */
    struct dma_buffer wr, rd;
    dma_alloc(&wr, 512, pci.dma_offset); dma_alloc(&rd, 512, pci.dma_offset);
    memcpy(wr.virt, "QueueEngine-M4-Test!", 20); dma_flush(wr.virt, 512);

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_IO_WRITE; cmd.nsid = 1; cmd.prp1 = wr.bus;
    cmd.cdw10 = (uint32_t)TEST_LBA; cmd.cdw11 = (uint32_t)(TEST_LBA>>32);
    nvme_qpair_submit_sync(&io, &cmd, NULL, 5000);

    memset(rd.virt, 0xAA, 512); dma_flush(rd.virt, 512);
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_IO_READ; cmd.nsid = 1; cmd.prp1 = rd.bus;
    cmd.cdw10 = (uint32_t)TEST_LBA; cmd.cdw11 = (uint32_t)(TEST_LBA>>32);
    nvme_qpair_submit_sync(&io, &cmd, NULL, 5000);

    dma_invalidate(rd.virt, 512);
    int ok = memcmp(wr.virt, rd.virt, 512) == 0;
    log_msg(LOG_INFO, "Write+Read+Verify: %s  (\"%.20s\")", ok ? "PASS" : "FAIL", (char*)rd.virt);
    if (!ok) rc = 1;

    dma_free(&wr); dma_free(&rd); dma_free(&buf);

shutdown:
    mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);
    nvme_qpair_destroy(&io); nvme_qpair_destroy(&admin);
cleanup:
    pci_unmap_bar0(&pci);
rebind:
    pci_rebind_nvme_driver(&pci);
done:
    log_shutdown(); return rc;
}
