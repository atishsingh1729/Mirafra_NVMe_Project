/*
 * milestone9.c — Validation
 * Data integrity checks, 5-second sustained load, performance stats.
 */
#include "log.h"
#include "pci.h"
#include "mmio.h"
#include "dma.h"
#include "nvme_regs.h"
#include "nvme_queue.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define ADM_DEPTH 32
#define IO_DEPTH  64
#define BASE_LBA  0x300000ULL

static long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

/* Write one block, read it back, compare byte-by-byte */
static int verify_pattern(struct nvme_qpair *io, struct dma_buffer *wr,
                          struct dma_buffer *rd, uint64_t lba, uint32_t bs,
                          const char *name) {
    struct nvme_cmd cmd = {0};

    dma_flush(wr->virt, bs);
    cmd.opcode = NVME_IO_WRITE; cmd.nsid = 1; cmd.prp1 = wr->bus;
    cmd.cdw10 = (uint32_t)(lba); cmd.cdw12 = 0;
    if (nvme_qpair_submit_sync(io, &cmd, NULL, 5000) != 0) return -1;

    memset(rd->virt, 0xDE, bs);
    dma_flush(rd->virt, bs);
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_IO_READ; cmd.nsid = 1; cmd.prp1 = rd->bus;
    cmd.cdw10 = (uint32_t)(lba); cmd.cdw12 = 0;
    if (nvme_qpair_submit_sync(io, &cmd, NULL, 5000) != 0) return -1;

    dma_invalidate(rd->virt, bs);

    uint8_t *w = wr->virt, *r = rd->virt;
    int bad = 0;
    for (uint32_t i = 0; i < bs; i++) {
        if (w[i] != r[i]) {
            if (bad < 3)
                log_msg(LOG_ERROR, "    mismatch at byte %u: wrote 0x%02X read 0x%02X", i, w[i], r[i]);
            bad++;
        }
    }

    if (bad == 0) log_msg(LOG_INFO, "  %-20s PASS", name);
    else          log_msg(LOG_ERROR, "  %-20s FAIL (%d bytes differ)", name, bad);
    return bad ? -1 : 0;
}

int main(int argc, char *argv[]) {
    struct pci_device pci;
    struct nvme_qpair admin, io;
    int rc = 0, failures = 0;

    log_init((argc > 1 && argv[1][0] == '-' && argv[1][1] == 'q')
             ? LOG_INFO : LOG_TRACE, "nvme_m9.log");

    /* standard setup */
    if (pci_find_nvme(&pci) < 0) { rc = 1; goto done; }
    pci_detect_dma_offset(&pci);
    pci_unbind_driver(&pci);
    pci_enable_bus_master(&pci);
    if (pci_map_bar0(&pci) < 0) { rc = 1; goto rebind; }
    mmio_init(pci.bar0);

    uint64_t cap = mmio_read64(NVME_REG_CAP);
    uint32_t dstrd = CAP_DSTRD(cap);
    uint32_t bs = 512;

    mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);
    while (mmio_read32(NVME_REG_CSTS) & CSTS_RDY) usleep(1000);

    nvme_qpair_create(&admin, 0, ADM_DEPTH, dstrd, pci.dma_offset);
    mmio_write32(NVME_REG_AQA, ((ADM_DEPTH-1) << 16) | (ADM_DEPTH-1));
    mmio_write64(NVME_REG_ASQ, nvme_qpair_sq_bus(&admin));
    mmio_write64(NVME_REG_ACQ, nvme_qpair_cq_bus(&admin));
    mmio_write32(NVME_REG_CC, CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_IOSQES_64 | CC_IOCQES_16);
    while (!(mmio_read32(NVME_REG_CSTS) & CSTS_RDY)) usleep(1000);

    struct nvme_cmd cmd = {0};
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

    struct dma_buffer wr, rd;
    dma_alloc(&wr, bs, pci.dma_offset);
    dma_alloc(&rd, bs, pci.dma_offset);

    /* ── Test 1: data integrity with different patterns ── */
    log_msg(LOG_INFO, "Test 1: Data integrity — 6 patterns, write+read+verify each");

    /* all zeros */
    memset(wr.virt, 0x00, bs);
    if (verify_pattern(&io, &wr, &rd, BASE_LBA, bs, "All zeros") < 0) failures++;

    /* all ones */
    memset(wr.virt, 0xFF, bs);
    if (verify_pattern(&io, &wr, &rd, BASE_LBA + 1, bs, "All 0xFF") < 0) failures++;

    /* alternating 0xAA / 0x55 */
    for (uint32_t i = 0; i < bs; i++) ((uint8_t*)wr.virt)[i] = (i % 2) ? 0x55 : 0xAA;
    if (verify_pattern(&io, &wr, &rd, BASE_LBA + 2, bs, "0xAA/0x55 alternate") < 0) failures++;

    /* sequential bytes */
    for (uint32_t i = 0; i < bs; i++) ((uint8_t*)wr.virt)[i] = (uint8_t)(i & 0xFF);
    if (verify_pattern(&io, &wr, &rd, BASE_LBA + 3, bs, "Sequential 0x00-0xFF") < 0) failures++;

    /* walking ones: 0x01 0x02 0x04 0x08 ... */
    for (uint32_t i = 0; i < bs; i++) ((uint8_t*)wr.virt)[i] = (uint8_t)(1 << (i % 8));
    if (verify_pattern(&io, &wr, &rd, BASE_LBA + 4, bs, "Walking ones") < 0) failures++;

    /* ASCII string */
    memset(wr.virt, 0, bs);
    snprintf(wr.virt, bs, "NVMe firmware validation — milestone 9 pattern test at LBA 0x%llX",
             (unsigned long long)(BASE_LBA + 5));
    if (verify_pattern(&io, &wr, &rd, BASE_LBA + 5, bs, "ASCII string") < 0) failures++;

    log_msg(LOG_INFO, "  Patterns: %d/6 passed", 6 - failures);

    /* ── Test 2: sustained load — 5 seconds of writes at QD=4 ── */
    log_msg(LOG_INFO, "Test 2: Sustained write load — 5 seconds at QD=4");

    struct dma_buffer io_bufs[4];
    for (int i = 0; i < 4; i++) {
        dma_alloc(&io_bufs[i], bs, pci.dma_offset);
        memset(io_bufs[i].virt, 0xCC + i, bs);
        dma_flush(io_bufs[i].virt, bs);
    }

    long start = now_us();
    long end = start + 5000000;  /* 5 seconds */
    int total_wr = 0, wr_errors = 0;
    int sub = 0;

    while (now_us() < end) {
        while (nvme_qpair_in_flight(&io) < 4 && now_us() < end) {
            int slot = sub % 4;
            dma_flush(io_bufs[slot].virt, bs);
            memset(&cmd, 0, sizeof(cmd));
            cmd.opcode = NVME_IO_WRITE; cmd.nsid = 1;
            cmd.prp1 = io_bufs[slot].bus;
            cmd.cdw10 = (uint32_t)(BASE_LBA + 100 + (sub % 1000));
            cmd.cdw12 = 0;
            if (nvme_qpair_submit(&io, &cmd) < 0) break;
            sub++;
        }
        nvme_qpair_poll(&io);
        for (int c = 0; c < io.depth; c++) {
            uint16_t st;
            if (nvme_qpair_cid_done(&io, c, &st, NULL)) {
                total_wr++;
                if (st != 0) wr_errors++;
                io.tracker[c].status = 0xFFFF;
            }
        }
    }
    /* drain remaining */
    while (nvme_qpair_in_flight(&io) > 0) {
        nvme_qpair_poll(&io);
        for (int c = 0; c < io.depth; c++) {
            uint16_t st;
            if (nvme_qpair_cid_done(&io, c, &st, NULL)) {
                total_wr++; if (st != 0) wr_errors++;
                io.tracker[c].status = 0xFFFF;
            }
        }
        usleep(10);
    }

    long elapsed_wr = now_us() - start;
    double wr_iops = (double)total_wr / ((double)elapsed_wr / 1e6);
    double wr_mbs = wr_iops * bs / (1024.0 * 1024.0);
    log_msg(LOG_INFO, "  %d writes in %.1f s — %.0f IOPS, %.2f MB/s, %d errors",
            total_wr, elapsed_wr / 1e6, wr_iops, wr_mbs, wr_errors);

    /* ── Test 3: sustained read load — 5 seconds at QD=4 ── */
    log_msg(LOG_INFO, "Test 3: Sustained read load — 5 seconds at QD=4");

    start = now_us();
    end = start + 5000000;
    int total_rd = 0, rd_errors = 0;
    sub = 0;

    while (now_us() < end) {
        while (nvme_qpair_in_flight(&io) < 4 && now_us() < end) {
            int slot = sub % 4;
            memset(io_bufs[slot].virt, 0, bs);
            dma_flush(io_bufs[slot].virt, bs);
            memset(&cmd, 0, sizeof(cmd));
            cmd.opcode = NVME_IO_READ; cmd.nsid = 1;
            cmd.prp1 = io_bufs[slot].bus;
            cmd.cdw10 = (uint32_t)(BASE_LBA + 100 + (sub % 1000));
            cmd.cdw12 = 0;
            if (nvme_qpair_submit(&io, &cmd) < 0) break;
            sub++;
        }
        nvme_qpair_poll(&io);
        for (int c = 0; c < io.depth; c++) {
            uint16_t st;
            if (nvme_qpair_cid_done(&io, c, &st, NULL)) {
                total_rd++;
                if (st != 0) rd_errors++;
                io.tracker[c].status = 0xFFFF;
            }
        }
    }
    while (nvme_qpair_in_flight(&io) > 0) {
        nvme_qpair_poll(&io);
        for (int c = 0; c < io.depth; c++) {
            uint16_t st;
            if (nvme_qpair_cid_done(&io, c, &st, NULL)) {
                total_rd++; if (st != 0) rd_errors++;
                io.tracker[c].status = 0xFFFF;
            }
        }
        usleep(10);
    }

    long elapsed_rd = now_us() - start;
    double rd_iops = (double)total_rd / ((double)elapsed_rd / 1e6);
    double rd_mbs = rd_iops * bs / (1024.0 * 1024.0);
    log_msg(LOG_INFO, "  %d reads in %.1f s — %.0f IOPS, %.2f MB/s, %d errors",
            total_rd, elapsed_rd / 1e6, rd_iops, rd_mbs, rd_errors);

    /* ── Test 4: post-load data integrity ── */
    log_msg(LOG_INFO, "Test 4: Data integrity after sustained load");
    memset(wr.virt, 0, bs);
    snprintf(wr.virt, bs, "Post-load integrity check");
    int post = verify_pattern(&io, &wr, &rd, BASE_LBA + 2000, bs, "Post-load pattern");
    if (post < 0) failures++;

    /* ── Test 5: SMART check after load ── */
    log_msg(LOG_INFO, "Test 5: SMART health after load");
    struct dma_buffer smart_buf;
    dma_alloc(&smart_buf, 512, pci.dma_offset);
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_GET_LOG_PAGE; cmd.nsid = 0xFFFFFFFF;
    cmd.prp1 = smart_buf.bus; cmd.cdw10 = (127 << 16) | 0x02;
    int sf = nvme_qpair_submit_sync(&admin, &cmd, NULL, 5000);
    if (sf == 0) {
        dma_invalidate(smart_buf.virt, 512);
        uint8_t *s = smart_buf.virt;
        uint16_t temp_k; memcpy(&temp_k, s + 1, 2);
        log_msg(LOG_INFO, "  %d°C | spare=%u%% | used=%u%% | warn=0x%02X",
                (int)temp_k - 273, s[3], s[5], s[0]);
    }
    dma_free(&smart_buf);

    /* summary */
    log_msg(LOG_INFO, "Summary:");
    log_msg(LOG_INFO, "  Integrity  : %d/6 patterns + post-load = %s",
            6 - failures, failures == 0 ? "ALL PASS" : "SOME FAILED");
    log_msg(LOG_INFO, "  Write load : %d cmds, %.0f IOPS, %.2f MB/s, %d errors",
            total_wr, wr_iops, wr_mbs, wr_errors);
    log_msg(LOG_INFO, "  Read load  : %d cmds, %.0f IOPS, %.2f MB/s, %d errors",
            total_rd, rd_iops, rd_mbs, rd_errors);
    if (wr_errors == 0 && rd_errors == 0 && failures == 0)
        log_msg(LOG_INFO, "  Verdict    : PASS — all tests clean");
    else
        log_msg(LOG_INFO, "  Verdict    : FAIL — %d issues", wr_errors + rd_errors + failures);

    for (int i = 0; i < 4; i++) dma_free(&io_bufs[i]);
    dma_free(&wr); dma_free(&rd);
    mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);
    nvme_qpair_destroy(&io);
    nvme_qpair_destroy(&admin);
    pci_unmap_bar0(&pci);
rebind:
    pci_rebind_nvme_driver(&pci);
done:
    log_shutdown();
    return rc;
}
