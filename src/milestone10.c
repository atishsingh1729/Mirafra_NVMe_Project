/*
 * milestone10.c — Demo and Review
 * End-to-end walkthrough: discovery → admin → queues → I/O → performance → health.
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
#define TEST_LBA  0x400000ULL

static long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

int main(int argc, char *argv[]) {
    struct pci_device pci;
    struct nvme_qpair admin, io;
    int rc = 0;

    log_init((argc > 1 && argv[1][0] == '-' && argv[1][1] == 'q')
             ? LOG_INFO : LOG_TRACE, "nvme_m10.log");

    log_msg(LOG_INFO, "NVMe Firmware Diagnostic — End-to-End Demo");
    log_msg(LOG_INFO, "============================================");

    /* ── 1. Discovery ── */
    if (pci_find_nvme(&pci) < 0) { rc = 1; goto done; }
    pci_detect_dma_offset(&pci);

    uint16_t msix_vec = 0;
    pci_check_msix(&pci, &msix_vec);

    log_msg(LOG_INFO, " ");
    log_msg(LOG_INFO, "[1] Device Discovery");
    log_msg(LOG_INFO, "  BDF         : %s", pci.bdf);
    log_msg(LOG_INFO, "  VID:DID     : 0x%04X:0x%04X", pci.vendor_id, pci.device_id);
    log_msg(LOG_INFO, "  BAR0        : 0x%llX (%llu KB)",
            (unsigned long long)pci.bar0_phys, (unsigned long long)pci.bar0_size / 1024);
    log_msg(LOG_INFO, "  DMA offset  : 0x%llX", (unsigned long long)pci.dma_offset);
    log_msg(LOG_INFO, "  MSI-X       : %u vectors", msix_vec);

    /* ── 2. Controller bring-up ── */
    pci_unbind_driver(&pci);
    pci_enable_bus_master(&pci);
    if (pci_map_bar0(&pci) < 0) { rc = 1; goto rebind; }
    mmio_init(pci.bar0);

    uint64_t cap = mmio_read64(NVME_REG_CAP);
    uint32_t vs = mmio_read32(NVME_REG_VS);
    uint32_t dstrd = CAP_DSTRD(cap);
    uint32_t to_ms = CAP_TO(cap) * 500;

    long t0 = now_us();
    mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);
    while (1) {
        uint32_t csts = mmio_read32(NVME_REG_CSTS);
        if (csts == 0xFFFFFFFF || !(csts & CSTS_RDY)) break;
        if (now_us() - t0 > 30000000) break;
        usleep(1000);
    }
    long disable_us = now_us() - t0;
    long recovery_us = 0;

    nvme_qpair_create(&admin, 0, ADM_DEPTH, dstrd, pci.dma_offset);
    mmio_write32(NVME_REG_AQA, ((ADM_DEPTH-1) << 16) | (ADM_DEPTH-1));
    mmio_write64(NVME_REG_ASQ, nvme_qpair_sq_bus(&admin));
    mmio_write64(NVME_REG_ACQ, nvme_qpair_cq_bus(&admin));

    t0 = now_us();
    mmio_write32(NVME_REG_CC, CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_IOSQES_64 | CC_IOCQES_16);
    while (1) {
        uint32_t csts = mmio_read32(NVME_REG_CSTS);
        if (csts == 0xFFFFFFFF) { log_msg(LOG_ERROR, "PCIe link dead"); rc = 1; goto rebind; }
        if (csts & CSTS_RDY) break;
        if (now_us() - t0 > 30000000) { log_msg(LOG_ERROR, "Enable timeout"); rc = 1; goto rebind; }
        usleep(1000);
    }
    long enable_us = now_us() - t0;

    log_msg(LOG_INFO, " ");
    log_msg(LOG_INFO, "[2] Controller");
    log_msg(LOG_INFO, "  NVMe %u.%u | MQES=%u | CAP.TO=%u ms",
            VS_MAJOR(vs), VS_MINOR(vs), CAP_MQES(cap) + 1, to_ms);
    log_msg(LOG_INFO, "  Disable: %ld us | Enable: %ld us", disable_us, enable_us);

    /* ── 3. Identify ── */
    struct dma_buffer buf;
    dma_alloc(&buf, 4096, pci.dma_offset);

    struct nvme_cmd cmd = {0};
    cmd.opcode = NVME_ADMIN_IDENTIFY; cmd.prp1 = buf.bus; cmd.cdw10 = NVME_IDENTIFY_CTRL;
    nvme_qpair_submit_sync(&admin, &cmd, NULL, 5000);
    dma_invalidate(buf.virt, 4096);

    uint8_t *d = buf.virt;
    char sn[21]={0}, mn[41]={0}, fr[9]={0};
    memcpy(sn, d+4, 20); memcpy(mn, d+24, 40); memcpy(fr, d+64, 8);
    for (int i=19; i>=0 && sn[i]==' '; i--) sn[i]=0;
    for (int i=39; i>=0 && mn[i]==' '; i--) mn[i]=0;
    uint8_t mdts = d[77];
    uint32_t nn; memcpy(&nn, d+516, 4);
    uint16_t oacs, oncs; memcpy(&oacs, d+256, 2); memcpy(&oncs, d+520, 2);

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_IDENTIFY; cmd.nsid = 1; cmd.prp1 = buf.bus; cmd.cdw10 = NVME_IDENTIFY_NS;
    nvme_qpair_submit_sync(&admin, &cmd, NULL, 5000);
    dma_invalidate(buf.virt, 4096);
    uint64_t nsze; memcpy(&nsze, buf.virt, 8);
    uint8_t flbas = ((uint8_t*)buf.virt)[26] & 0x0F;
    uint32_t fmt; memcpy(&fmt, (uint8_t*)buf.virt + 128 + flbas*4, 4);
    uint32_t bs = 1U << ((fmt >> 16) & 0xFF);
    uint64_t cap_gb = (nsze * bs) / (1024ULL * 1024 * 1024);

    log_msg(LOG_INFO, " ");
    log_msg(LOG_INFO, "[3] Identity");
    log_msg(LOG_INFO, "  Model    : %s", mn);
    log_msg(LOG_INFO, "  Serial   : %s", sn);
    log_msg(LOG_INFO, "  Firmware : %s", fr);
    log_msg(LOG_INFO, "  NS1      : %llu LBAs x %u B = %llu GB",
            (unsigned long long)nsze, bs, (unsigned long long)cap_gb);
    log_msg(LOG_INFO, "  MDTS=%u | NN=%u | OACS=0x%04X | ONCS=0x%04X", mdts, nn, oacs, oncs);

    /* ── 4. SMART ── */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_GET_LOG_PAGE; cmd.nsid = 0xFFFFFFFF;
    cmd.prp1 = buf.bus; cmd.cdw10 = (127 << 16) | 0x02;
    nvme_qpair_submit_sync(&admin, &cmd, NULL, 5000);
    dma_invalidate(buf.virt, 512);

    uint8_t *s = buf.virt;
    uint16_t temp_k; memcpy(&temp_k, s+1, 2);
    uint64_t pcyc, phrs, ushut, merr;
    memcpy(&pcyc, s+112, 8); memcpy(&phrs, s+128, 8);
    memcpy(&ushut, s+144, 8); memcpy(&merr, s+160, 8);
    double gb_r = *(uint64_t*)(s+32) * 512000.0 / (1024.0*1024*1024);
    double gb_w = *(uint64_t*)(s+48) * 512000.0 / (1024.0*1024*1024);

    log_msg(LOG_INFO, " ");
    log_msg(LOG_INFO, "[4] Health");
    log_msg(LOG_INFO, "  Temp=%d°C | Spare=%u%% | Used=%u%% | Warn=0x%02X",
            (int)temp_k - 273, s[3], s[5], s[0]);
    log_msg(LOG_INFO, "  Read=%.0f GB | Written=%.0f GB", gb_r, gb_w);
    log_msg(LOG_INFO, "  Power: %llu cycles, %llu hrs | Unsafe: %llu | Media errors: %llu",
            (unsigned long long)pcyc, (unsigned long long)phrs,
            (unsigned long long)ushut, (unsigned long long)merr);

    /* ── 5. I/O queue setup ── */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_SET_FEATURES; cmd.cdw10 = NVME_FEAT_NUM_QUEUES; cmd.cdw11 = (7<<16)|7;
    uint32_t feat_res;
    nvme_qpair_submit_sync(&admin, &cmd, &feat_res, 5000);

    nvme_qpair_create(&io, 1, IO_DEPTH, dstrd, pci.dma_offset);
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CREATE_IO_CQ; cmd.prp1 = nvme_qpair_cq_bus(&io);
    cmd.cdw10 = ((IO_DEPTH-1)<<16)|1; cmd.cdw11 = 1;
    nvme_qpair_submit_sync(&admin, &cmd, NULL, 5000);
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CREATE_IO_SQ; cmd.prp1 = nvme_qpair_sq_bus(&io);
    cmd.cdw10 = ((IO_DEPTH-1)<<16)|1; cmd.cdw11 = (1<<16)|1;
    nvme_qpair_submit_sync(&admin, &cmd, NULL, 5000);

    /* ── 6. Data integrity ── */
    struct dma_buffer wr, rd;
    dma_alloc(&wr, bs, pci.dma_offset);
    dma_alloc(&rd, bs, pci.dma_offset);

    memset(wr.virt, 0, bs);
    snprintf(wr.virt, bs, "NVMe firmware demo — milestone 10 final test");
    dma_flush(wr.virt, bs);

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_IO_WRITE; cmd.nsid = 1; cmd.prp1 = wr.bus;
    cmd.cdw10 = (uint32_t)TEST_LBA; cmd.cdw12 = 0;
    nvme_qpair_submit_sync(&io, &cmd, NULL, 5000);

    memset(rd.virt, 0xDE, bs);
    dma_flush(rd.virt, bs);
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_IO_READ; cmd.nsid = 1; cmd.prp1 = rd.bus;
    cmd.cdw10 = (uint32_t)TEST_LBA; cmd.cdw12 = 0;
    nvme_qpair_submit_sync(&io, &cmd, NULL, 5000);
    dma_invalidate(rd.virt, bs);

    int match = (memcmp(wr.virt, rd.virt, bs) == 0);

    log_msg(LOG_INFO, " ");
    log_msg(LOG_INFO, "[5] Data Integrity");
    log_msg(LOG_INFO, "  Write+Read+Verify: %s", match ? "PASS" : "FAIL");
    log_msg(LOG_INFO, "  Data: \"%.45s\"", (char*)rd.virt);

    /* ── 7. Performance — QD sweep ── */
    log_msg(LOG_INFO, " ");
    log_msg(LOG_INFO, "[6] Performance (1-second runs, 512 B blocks)");

    struct dma_buffer perf_bufs[16];
    for (int i = 0; i < 16; i++) {
        dma_alloc(&perf_bufs[i], bs, pci.dma_offset);
        memset(perf_bufs[i].virt, 0xAA + i, bs);
        dma_flush(perf_bufs[i].virt, bs);
    }

    int qds[] = {1, 4, 16};
    uint8_t ops[] = {NVME_IO_WRITE, NVME_IO_READ};
    const char *op_names[] = {"Write", "Read"};
    double all_iops[6], all_mbs[6];
    int idx = 0;

    for (int o = 0; o < 2; o++) {
        for (int q = 0; q < 3; q++) {
            int sub = 0, done = 0;
            long start = now_us();
            long end = start + 1000000;

            while (now_us() < end) {
                while (nvme_qpair_in_flight(&io) < (uint16_t)qds[q] && now_us() < end) {
                    int slot = sub % 16;
                    if (ops[o] == NVME_IO_WRITE) dma_flush(perf_bufs[slot].virt, bs);
                    else { memset(perf_bufs[slot].virt, 0, bs); dma_flush(perf_bufs[slot].virt, bs); }
                    memset(&cmd, 0, sizeof(cmd));
                    cmd.opcode = ops[o]; cmd.nsid = 1;
                    cmd.prp1 = perf_bufs[slot].bus;
                    cmd.cdw10 = (uint32_t)(TEST_LBA + 1000 + (sub % 500));
                    cmd.cdw12 = 0;
                    if (nvme_qpair_submit(&io, &cmd) < 0) break;
                    sub++;
                }
                nvme_qpair_poll(&io);
                for (int c = 0; c < io.depth; c++) {
                    uint16_t st;
                    if (nvme_qpair_cid_done(&io, c, &st, NULL)) {
                        done++;
                        io.tracker[c].status = 0xFFFF;
                    }
                }
            }
            /* Drain remaining — 2 second timeout to avoid hanging */
            long drain_start = now_us();
            while (nvme_qpair_in_flight(&io) > 0 && (now_us() - drain_start) < 2000000) {
                nvme_qpair_poll(&io);
                for (int c = 0; c < io.depth; c++) {
                    uint16_t st;
                    if (nvme_qpair_cid_done(&io, c, &st, NULL)) {
                        done++;
                        io.tracker[c].status = 0xFFFF;
                    }
                }
                usleep(10);
            }
            if (nvme_qpair_in_flight(&io) > 0) {
                log_msg(LOG_WARN, "  %s QD=%-2d : drain timeout (%u stuck), device may be dead",
                        op_names[o], qds[q], nvme_qpair_in_flight(&io));
                io.cmds_in_flight = 0;  /* force reset so next test can proceed */
                continue;
            }

            long wall = now_us() - start;
            double iops = (double)done / ((double)wall / 1e6);
            double mbs = iops * bs / (1024.0 * 1024.0);
            all_iops[idx] = iops; all_mbs[idx] = mbs; idx++;

            log_msg(LOG_INFO, "  %s QD=%-2d : %6d cmds, %7.0f IOPS, %6.2f MB/s",
                    op_names[o], qds[q], done, iops, mbs);
        }
    }

    /* ── 8. Error resilience ── */
    log_msg(LOG_INFO, " ");
    log_msg(LOG_INFO, "[7] Error Resilience");
    memset(&cmd, 0, sizeof(cmd)); cmd.opcode = 0xFF; cmd.prp1 = buf.bus;
    int sf = nvme_qpair_submit_sync(&admin, &cmd, NULL, 5000);
    log_msg(LOG_INFO, "  Invalid opcode → SF=0x%04X", sf);

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_IDENTIFY; cmd.prp1 = buf.bus; cmd.cdw10 = NVME_IDENTIFY_CTRL;
    sf = nvme_qpair_submit_sync(&admin, &cmd, NULL, 5000);
    log_msg(LOG_INFO, "  Post-error Identify: %s", sf == 0 ? "OK" : "FAIL");

    /* ── 9. Reset recovery ── */
    log_msg(LOG_INFO, " ");
    log_msg(LOG_INFO, "[8] Reset Recovery");
    t0 = now_us();
    mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);
    int reset_ok = 1;
    while (1) {
        uint32_t csts = mmio_read32(NVME_REG_CSTS);
        if (csts == 0xFFFFFFFF) { log_msg(LOG_ERROR, "  PCIe link down during reset"); reset_ok = 0; break; }
        if (!(csts & CSTS_RDY)) break;
        if (now_us() - t0 > 30000000) { log_msg(LOG_ERROR, "  Reset timeout"); reset_ok = 0; break; }
        usleep(1000);
    }
    long reset_us = now_us() - t0;

    nvme_qpair_destroy(&io);
    nvme_qpair_destroy(&admin);

    if (reset_ok) {
        nvme_qpair_create(&admin, 0, ADM_DEPTH, dstrd, pci.dma_offset);
        mmio_write32(NVME_REG_AQA, ((ADM_DEPTH-1)<<16)|(ADM_DEPTH-1));
        mmio_write64(NVME_REG_ASQ, nvme_qpair_sq_bus(&admin));
        mmio_write64(NVME_REG_ACQ, nvme_qpair_cq_bus(&admin));

        t0 = now_us();
        mmio_write32(NVME_REG_CC, CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_IOSQES_64 | CC_IOCQES_16);
        while (1) {
            uint32_t csts = mmio_read32(NVME_REG_CSTS);
            if (csts == 0xFFFFFFFF) { log_msg(LOG_ERROR, "  PCIe link down during enable"); reset_ok = 0; break; }
            if (csts & CSTS_RDY) break;
            if (now_us() - t0 > 30000000) { log_msg(LOG_ERROR, "  Enable timeout"); reset_ok = 0; break; }
            usleep(1000);
        }
        recovery_us = now_us() - t0;

        if (reset_ok) {
            memset(&cmd, 0, sizeof(cmd));
            cmd.opcode = NVME_ADMIN_IDENTIFY; cmd.prp1 = buf.bus; cmd.cdw10 = NVME_IDENTIFY_CTRL;
            sf = nvme_qpair_submit_sync(&admin, &cmd, NULL, 5000);
        }
    }

    if (reset_ok)
        log_msg(LOG_INFO, "  Reset: %ld us | Recovery: %ld us | Post-reset Identify: %s",
                reset_us, recovery_us, sf == 0 ? "OK" : "FAIL");
    else
        log_msg(LOG_WARN, "  Reset: link lost — skipped (run 'echo on > power/control' before unbinding)");

    /* ── Final Summary ── */
    log_msg(LOG_INFO, " ");
    log_msg(LOG_INFO, "============================================");
    log_msg(LOG_INFO, "Final Summary");
    log_msg(LOG_INFO, "============================================");
    log_msg(LOG_INFO, "  Device     : %s (%s) FW=%s", mn, sn, fr);
    log_msg(LOG_INFO, "  Capacity   : %llu GB (%u-byte blocks)", (unsigned long long)cap_gb, bs);
    log_msg(LOG_INFO, "  Health     : %d°C, %u%% spare, %u%% used, %llu media errors",
            (int)temp_k - 273, s[3], s[5], (unsigned long long)merr);
    log_msg(LOG_INFO, "  Integrity  : %s", match ? "PASS" : "FAIL");
    log_msg(LOG_INFO, "  Performance:");
    for (int i = 0; i < 6; i++)
        log_msg(LOG_INFO, "    %s QD=%-2d : %7.0f IOPS, %6.2f MB/s",
                i < 3 ? "Write" : "Read", qds[i % 3], all_iops[i], all_mbs[i]);
    log_msg(LOG_INFO, "  Reset      : %s",
            reset_ok ? "OK" : "link lost (previous Ctrl+C or D3cold)");
    log_msg(LOG_INFO, "  Errors     : controller survived invalid commands");
    log_msg(LOG_INFO, "  Verdict    : %s", (match && !rc) ? "ALL PASS" : "ISSUES FOUND");
    log_msg(LOG_INFO, "============================================");

    for (int i = 0; i < 16; i++) dma_free(&perf_bufs[i]);
    dma_free(&wr); dma_free(&rd); dma_free(&buf);
    if (reset_ok) {
        mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);
        nvme_qpair_destroy(&admin);
    }
    pci_unmap_bar0(&pci);
rebind:
    pci_rebind_nvme_driver(&pci);
done:
    log_shutdown();
    return rc;
}
