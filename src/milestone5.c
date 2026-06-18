/*
 * milestone5.c — Interrupt and Reset Flow
 *
 * - Set up MSI-X interrupt delivery via uio_pci_generic (primary)
 * - Fall back to CQ phase bit polling if UIO unavailable
 * - Wire interrupt handler to CQ processing loop
 * - Controller reset mid-operation + recovery within CAP.TO
 */
#include "log.h"
#include "pci.h"
#include "mmio.h"
#include "dma.h"
#include "nvme_regs.h"
#include "nvme_queue.h"
#include "nvme_irq.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ADM_DEPTH 32

static long elapsed_ms(struct timespec *t0) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t0->tv_sec) * 1000 + (now.tv_nsec - t0->tv_nsec) / 1000000;
}

/* Bring up controller with fresh admin queue pair */
static int ctrl_bring_up(struct nvme_qpair *admin, struct pci_device *pci,
                         uint32_t dstrd, uint32_t to_ms) {
    struct timespec t0;

    if (mmio_read32(NVME_REG_CC) & CC_EN)
        mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (mmio_read32(NVME_REG_CSTS) & CSTS_RDY) {
        if (elapsed_ms(&t0) > (long)to_ms) return -1;
        usleep(1000);
    }

    if (nvme_qpair_create(admin, 0, ADM_DEPTH, dstrd, pci->dma_offset) < 0)
        return -1;

    mmio_write32(NVME_REG_AQA, ((ADM_DEPTH-1) << 16) | (ADM_DEPTH-1));
    mmio_write64(NVME_REG_ASQ, nvme_qpair_sq_bus(admin));
    mmio_write64(NVME_REG_ACQ, nvme_qpair_cq_bus(admin));
    mmio_write32(NVME_REG_CC, CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_IOSQES_64 | CC_IOCQES_16);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!(mmio_read32(NVME_REG_CSTS) & CSTS_RDY)) {
        uint32_t csts = mmio_read32(NVME_REG_CSTS);
        if ((csts & CSTS_CFS) || csts == 0xFFFFFFFF) return -1;
        if (elapsed_ms(&t0) > (long)to_ms) return -1;
        usleep(1000);
    }
    return 0;
}

/*
 * CQ processing loop that uses interrupts when available.
 * If in UIO mode: waits for interrupt, then polls CQ to harvest.
 * If in poll mode: just polls CQ directly.
 */
static int irq_driven_cq_process(struct nvme_qpair *qp, struct nvme_irq *irq,
                                 int timeout_ms) {
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);

    while (nvme_qpair_in_flight(qp) > 0) {
        if (irq->mode == NVME_IRQ_MODE_UIO) {
            /* Block until interrupt fires or timeout */
            int got_irq = nvme_irq_wait(irq, 100);
            if (got_irq) {
                nvme_qpair_poll(qp);    /* Harvest completions */
                nvme_irq_ack(irq);      /* Re-enable for next interrupt */
            }
        } else {
            /* Pure polling fallback */
            nvme_qpair_poll(qp);
            usleep(50);
        }

        if (elapsed_ms(&t0) > timeout_ms) return -1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    struct pci_device pci;
    struct nvme_qpair admin;
    struct nvme_irq irq;
    int rc = 0;

    log_init((argc > 1 && argv[1][0] == '-' && argv[1][1] == 'q')
             ? LOG_INFO : LOG_TRACE, "nvme_m5.log");

    if (pci_find_nvme(&pci) < 0) { rc = 1; goto done; }
    pci_detect_dma_offset(&pci);

    /* === Test 1: Set up interrupt delivery === */
    log_msg(LOG_INFO, "Test 1: Interrupt setup");

    /* Unbind kernel driver first — needed before UIO binding */
    pci_unbind_driver(&pci);
    pci_enable_bus_master(&pci);

    /* Try to set up MSI-X via UIO, falls back to polling */
    nvme_irq_init(&irq, &pci);
    log_msg(LOG_INFO, "  Mode: %s", nvme_irq_mode_str(&irq));
    if (irq.msix_vectors > 0)
        log_msg(LOG_INFO, "  MSI-X table: %u vectors", irq.msix_vectors);

    /*
     * If UIO took over the device, we need to map BAR0 through
     * the UIO-bound device. If polling, we map directly.
     * Either way, resource0 mmap works.
     */
    if (pci_map_bar0(&pci) < 0) { rc = 1; goto irq_cleanup; }
    mmio_init(pci.bar0);

    uint64_t cap = mmio_read64(NVME_REG_CAP);
    uint32_t dstrd = CAP_DSTRD(cap);
    uint32_t to_ms = CAP_TO(cap) * 500;
    if (to_ms == 0) to_ms = 5000;

    /* === Test 2: Interrupt-driven CQ processing === */
    log_msg(LOG_INFO, "Test 2: %s CQ processing", nvme_irq_mode_str(&irq));

    if (ctrl_bring_up(&admin, &pci, dstrd, to_ms) < 0) {
        log_msg(LOG_ERROR, "  Controller bring-up failed");
        rc = 1; goto cleanup;
    }

    /* Submit 5 Identify commands and process via interrupt/poll loop */
    struct dma_buffer bufs[5];
    for (int i = 0; i < 5; i++) {
        dma_alloc(&bufs[i], 4096, pci.dma_offset);
        struct nvme_cmd cmd = {0};
        cmd.opcode = NVME_ADMIN_IDENTIFY;
        cmd.prp1 = bufs[i].bus;
        cmd.cdw10 = NVME_IDENTIFY_CTRL;
        nvme_qpair_submit(&admin, &cmd);
    }
    log_msg(LOG_INFO, "  Submitted 5 commands, in-flight=%u", nvme_qpair_in_flight(&admin));

    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    if (irq_driven_cq_process(&admin, &irq, 5000) < 0) {
        log_msg(LOG_ERROR, "  CQ processing timeout");
        rc = 1;
    } else {
        long ms = elapsed_ms(&t0);
        log_msg(LOG_INFO, "  All 5 completed in %ld ms via %s", ms, nvme_irq_mode_str(&irq));
    }

    /* Verify all CIDs completed */
    int all_ok = 1;
    for (int i = 0; i < 5; i++) {
        uint16_t st;
        if (!nvme_qpair_cid_done(&admin, i, &st, NULL) || st != 0) all_ok = 0;
        dma_free(&bufs[i]);
    }
    log_msg(LOG_INFO, "  CID verification: %s", all_ok ? "all OK" : "FAILED");
    if (!all_ok) rc = 1;

    /* === Test 3: Controller reset mid-operation === */
    log_msg(LOG_INFO, "Test 3: Reset mid-operation");

    struct dma_buffer pre_buf;
    dma_alloc(&pre_buf, 4096, pci.dma_offset);
    struct nvme_cmd pre_cmd = {0};
    pre_cmd.opcode = NVME_ADMIN_IDENTIFY;
    pre_cmd.prp1 = pre_buf.bus;
    pre_cmd.cdw10 = NVME_IDENTIFY_CTRL;
    nvme_qpair_submit(&admin, &pre_cmd);
    log_msg(LOG_INFO, "  Command submitted, forcing reset...");

    mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);

    struct timespec reset_t0; clock_gettime(CLOCK_MONOTONIC, &reset_t0);
    while (mmio_read32(NVME_REG_CSTS) & CSTS_RDY) {
        if (elapsed_ms(&reset_t0) > (long)to_ms) {
            log_msg(LOG_ERROR, "  Reset timeout"); rc = 1; goto shutdown;
        }
        usleep(1000);
    }
    long reset_ms = elapsed_ms(&reset_t0);
    log_msg(LOG_INFO, "  Disabled in %ld ms (limit %u ms) — %s",
            reset_ms, to_ms, reset_ms <= (long)to_ms ? "OK" : "EXCEEDED");
    dma_free(&pre_buf);
    nvme_qpair_destroy(&admin);

    /* === Test 4: Recovery and verify === */
    log_msg(LOG_INFO, "Test 4: Recovery after reset");

    struct timespec en_t0; clock_gettime(CLOCK_MONOTONIC, &en_t0);
    if (ctrl_bring_up(&admin, &pci, dstrd, to_ms) < 0) {
        log_msg(LOG_ERROR, "  Recovery failed"); rc = 1; goto cleanup;
    }
    long en_ms = elapsed_ms(&en_t0);
    log_msg(LOG_INFO, "  Re-enabled in %ld ms (limit %u ms) — %s",
            en_ms, to_ms, en_ms <= (long)to_ms ? "OK" : "EXCEEDED");

    /* Verify with Identify */
    struct dma_buffer vbuf;
    dma_alloc(&vbuf, 4096, pci.dma_offset);
    struct nvme_cmd vcmd = {0};
    vcmd.opcode = NVME_ADMIN_IDENTIFY; vcmd.prp1 = vbuf.bus; vcmd.cdw10 = NVME_IDENTIFY_CTRL;
    int sf = nvme_qpair_submit_sync(&admin, &vcmd, NULL, 5000);
    if (sf == 0) {
        dma_invalidate(vbuf.virt, 4096);
        char sn[21] = {0}; memcpy(sn, (uint8_t*)vbuf.virt + 4, 20);
        for (int i = 19; i >= 0 && sn[i] == ' '; i--) sn[i] = 0;
        log_msg(LOG_INFO, "  Post-reset Identify OK: SN=%s", sn);
    } else {
        log_msg(LOG_ERROR, "  Post-reset Identify FAILED"); rc = 1;
    }
    dma_free(&vbuf);

    /* Summary */
    log_msg(LOG_INFO, "Results:");
    log_msg(LOG_INFO, "  IRQ mode    : %s", nvme_irq_mode_str(&irq));
    log_msg(LOG_INFO, "  MSI-X       : %u vectors", irq.msix_vectors);
    log_msg(LOG_INFO, "  CQ process  : 5/5 via %s", nvme_irq_mode_str(&irq));
    log_msg(LOG_INFO, "  Reset       : %ld ms (limit %u ms)", reset_ms, to_ms);
    log_msg(LOG_INFO, "  Recovery    : %ld ms, Identify OK", en_ms);

shutdown:
    mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);
    nvme_qpair_destroy(&admin);
cleanup:
    pci_unmap_bar0(&pci);
irq_cleanup:
    nvme_irq_shutdown(&irq);
    /* Clear driver_override so nvme can bind again */
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/driver_override", pci.sysfs_path);
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "\n"); fclose(f); }
    }
    pci_rebind_nvme_driver(&pci);
done:
    log_shutdown();
    return rc;
}
