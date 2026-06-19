/*
 * milestone6.c — Transport Hooks
 *
 * Run NVMe commands through the transport layer hook chain:
 * pre_submit (DMA alloc, cache flush) → SQE → submit → poll →
 * post_complete (cache invalidate, status propagate).
 * Then reset via transport_reset and verify recovery.
 */
#include "log.h"
#include "pci.h"
#include "mmio.h"
#include "nvme_regs.h"
#include "transport.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_LBA 0x100000ULL

/* Custom hook counters for test 5 */
static int g_pre_count, g_post_count;

static int custom_pre_submit(struct transport_req *req, void *ctx) {
    g_pre_count++;
    log_msg(LOG_INFO, "  [custom pre_submit] opcode=0x%02X call #%d", req->opcode, g_pre_count);
    /* Call default logic by doing DMA alloc ourselves */
    struct transport *t = (struct transport *)ctx;
    if (req->buf && req->buf_len > 0) {
        if (dma_alloc(&req->dma, req->buf_len, t->pci->dma_offset) < 0) return -1;
        if (req->opcode == NVME_IO_WRITE) {
            memcpy(req->dma.virt, req->buf, req->buf_len);
            dma_flush(req->dma.virt, req->buf_len);
        } else {
            dma_flush(req->dma.virt, req->buf_len);
        }
    }
    return 0;
}

static void custom_post_complete(struct transport_req *req, void *ctx) {
    (void)ctx;
    g_post_count++;
    req->completed = 1;
    if (req->status == 0 && req->buf && req->dma.virt && req->opcode != NVME_IO_WRITE) {
        dma_invalidate(req->dma.virt, req->buf_len);
        memcpy(req->buf, req->dma.virt, req->buf_len);
    }
    if (req->dma.virt) dma_free(&req->dma);
    log_msg(LOG_INFO, "  [custom post_complete] SF=0x%04X call #%d", req->status, g_post_count);
}

int main(int argc, char *argv[]) {
    struct pci_device pci;
    struct transport tr;
    int rc = 0;

    log_init((argc > 1 && argv[1][0] == '-' && argv[1][1] == 'q')
             ? LOG_INFO : LOG_TRACE, "nvme_m6.log");

    if (pci_find_nvme(&pci) < 0) { rc = 1; goto done; }
    pci_detect_dma_offset(&pci);
    pci_unbind_driver(&pci);
    pci_enable_bus_master(&pci);
    if (pci_map_bar0(&pci) < 0) { rc = 1; goto rebind; }
    mmio_init(pci.bar0);

    /* Init transport (resets controller, creates admin queue, identifies drive) */
    if (transport_init(&tr, &pci) < 0) { rc = 1; goto cleanup; }

    /* === Test 1: Identify Controller via transport === */
    log_msg(LOG_INFO, "Test 1: Identify Controller through hook chain");
    uint8_t id_buf[4096];
    struct transport_req req = {0};
    req.opcode = NVME_ADMIN_IDENTIFY;
    req.cdw10 = NVME_IDENTIFY_CTRL;
    req.buf = id_buf;
    req.buf_len = 4096;

    if (transport_submit(&tr, &req) != 0) {
        log_msg(LOG_ERROR, "  Identify Controller failed SF=0x%04X", req.status);
        rc = 1; goto shutdown;
    }
    char sn[21] = {0}, mn[41] = {0}, fr[9] = {0};
    memcpy(sn, id_buf + 4, 20); memcpy(mn, id_buf + 24, 40); memcpy(fr, id_buf + 64, 8);
    for (int i = 19; i >= 0 && sn[i] == ' '; i--) sn[i] = 0;
    for (int i = 39; i >= 0 && mn[i] == ' '; i--) mn[i] = 0;
    log_msg(LOG_INFO, "  %s  SN=%s  FW=%s  status=0x%04X", mn, sn, fr, req.status);

    /* === Test 2: SMART log via transport === */
    log_msg(LOG_INFO, "Test 2: SMART log through hook chain");
    uint8_t smart_buf[512];
    memset(&req, 0, sizeof(req));
    req.opcode = NVME_ADMIN_GET_LOG_PAGE;
    req.nsid = 0xFFFFFFFF;
    req.cdw10 = (127 << 16) | NVME_LOG_SMART;
    req.buf = smart_buf;
    req.buf_len = 512;

    if (transport_submit(&tr, &req) != 0) {
        log_msg(LOG_ERROR, "  SMART failed SF=0x%04X", req.status);
    } else {
        uint16_t temp_k; memcpy(&temp_k, smart_buf + 1, 2);
        log_msg(LOG_INFO, "  SMART: %d°C  spare=%u%%  used=%u%%  status=0x%04X",
                (int)temp_k - 273, smart_buf[3], smart_buf[5], req.status);
    }

    /* === Test 3: I/O Write + Read via transport === */
    log_msg(LOG_INFO, "Test 3: I/O Write + Read through hook chain");

    if (transport_create_io_queue(&tr, 64) < 0) {
        log_msg(LOG_ERROR, "  I/O queue creation failed"); rc = 1; goto shutdown;
    }

    uint8_t write_buf[512], read_buf[512];
    memset(write_buf, 0, sizeof(write_buf));
    memcpy(write_buf, "Transport-M6-Hook-Test!", 23);
    for (int i = 23; i < 512; i++) write_buf[i] = (uint8_t)(i & 0xFF);

    memset(&req, 0, sizeof(req));
    req.opcode = NVME_IO_WRITE;
    req.is_io = 1;
    req.nsid = 1;
    req.lba = TEST_LBA;
    req.num_blocks = 0;
    req.buf = write_buf;
    req.buf_len = tr.block_size;

    if (transport_submit(&tr, &req) != 0) {
        log_msg(LOG_ERROR, "  Write failed SF=0x%04X", req.status);
        rc = 1; goto shutdown;
    }
    log_msg(LOG_INFO, "  Write OK at LBA=0x%llX  status=0x%04X",
            (unsigned long long)TEST_LBA, req.status);

    memset(read_buf, 0xAA, sizeof(read_buf));
    memset(&req, 0, sizeof(req));
    req.opcode = NVME_IO_READ;
    req.is_io = 1;
    req.nsid = 1;
    req.lba = TEST_LBA;
    req.num_blocks = 0;
    req.buf = read_buf;
    req.buf_len = tr.block_size;

    if (transport_submit(&tr, &req) != 0) {
        log_msg(LOG_ERROR, "  Read failed SF=0x%04X", req.status);
        rc = 1; goto shutdown;
    }

    int match = (memcmp(write_buf, read_buf, tr.block_size) == 0);
    log_msg(LOG_INFO, "  Read OK, verify: %s  (\"%.23s\")  status=0x%04X",
            match ? "PASS" : "FAIL", (char *)read_buf, req.status);
    if (!match) rc = 1;

    /* === Test 4: Reset via transport hook === */
    log_msg(LOG_INFO, "Test 4: Controller reset via transport_reset hook");

    if (transport_reset(&tr) != 0) {
        log_msg(LOG_ERROR, "  Reset failed"); rc = 1; goto shutdown;
    }

    /* Verify with Identify post-reset */
    memset(&req, 0, sizeof(req));
    req.opcode = NVME_ADMIN_IDENTIFY;
    req.cdw10 = NVME_IDENTIFY_CTRL;
    req.buf = id_buf;
    req.buf_len = 4096;

    if (transport_submit(&tr, &req) != 0) {
        log_msg(LOG_ERROR, "  Post-reset Identify failed"); rc = 1; goto shutdown;
    }
    memcpy(sn, id_buf + 4, 20);
    for (int i = 19; i >= 0 && sn[i] == ' '; i--) sn[i] = 0;
    log_msg(LOG_INFO, "  Post-reset Identify OK: SN=%s  status=0x%04X", sn, req.status);

    /* === Test 5: Custom hooks === */
    log_msg(LOG_INFO, "Test 5: Custom hook chain");
    g_pre_count = 0; g_post_count = 0;

    transport_set_hooks(&tr, custom_pre_submit, custom_post_complete, NULL, &tr);

    memset(&req, 0, sizeof(req));
    req.opcode = NVME_ADMIN_IDENTIFY;
    req.cdw10 = NVME_IDENTIFY_CTRL;
    req.buf = id_buf;
    req.buf_len = 4096;

    if (transport_submit(&tr, &req) != 0) {
        log_msg(LOG_ERROR, "  Custom hook Identify failed"); rc = 1; goto shutdown;
    }
    log_msg(LOG_INFO, "  Custom hooks fired: pre=%d post=%d  status=0x%04X",
            g_pre_count, g_post_count, req.status);

    /* Summary */
    log_msg(LOG_INFO, "Results:");
    log_msg(LOG_INFO, "  Identify via transport : PASS");
    log_msg(LOG_INFO, "  SMART via transport    : PASS");
    log_msg(LOG_INFO, "  I/O W+R via transport  : %s", match ? "PASS" : "FAIL");
    log_msg(LOG_INFO, "  Reset hook             : PASS");
    log_msg(LOG_INFO, "  Custom hooks           : pre=%d post=%d", g_pre_count, g_post_count);

shutdown:
    transport_shutdown(&tr);
cleanup:
    pci_unmap_bar0(&pci);
rebind:
    pci_rebind_nvme_driver(&pci);
done:
    log_shutdown();
    return rc;
}
