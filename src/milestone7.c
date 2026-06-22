/*
 * milestone7.c — I/O Flow Simulation
 *
 * Repeated Read/Write commands with per-command latency tracking.
 * Tests at queue depths 1, 4, 16 to show concurrency effects.
 * Reports min/avg/p50/p99/max latency, IOPS, and MB/s.
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

#define ADM_DEPTH   32
#define IO_DEPTH    64
#define TEST_LBA    0x200000ULL  /* safe offset for testing */
#define NUM_CMDS    200         /* commands per test run */
#define MAX_INFLIGHT 16

struct latency_sample {
    long us;  /* microseconds */
};

static int cmp_long(const void *a, const void *b) {
    long la = ((const struct latency_sample *)a)->us;
    long lb = ((const struct latency_sample *)b)->us;
    return (la > lb) - (la < lb);
}

static long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

/* Run workload at a given queue depth */
static int run_workload(struct nvme_qpair *io, struct dma_buffer *bufs,
                        int num_bufs, uint8_t opcode, int queue_depth,
                        int total_cmds, uint32_t block_size,
                        struct latency_sample *samples) {
    int submitted = 0, completed = 0;
    long submit_time[MAX_INFLIGHT] = {0};
    int cid_to_slot[256];  /* map CID → slot in submit_time */
    memset(cid_to_slot, -1, sizeof(cid_to_slot));

    long wall_start = now_us();

    while (completed < total_cmds) {
        /* Submit up to queue_depth commands */
        while (submitted < total_cmds &&
               nvme_qpair_in_flight(io) < (uint16_t)queue_depth) {
            int slot = submitted % num_bufs;

            if (opcode == NVME_IO_WRITE)
                dma_flush(bufs[slot].virt, block_size);

            struct nvme_cmd cmd = {0};
            cmd.opcode = opcode;
            cmd.nsid = 1;
            cmd.prp1 = bufs[slot].bus;
            cmd.cdw10 = (uint32_t)((TEST_LBA + submitted) & 0xFFFFFFFF);
            cmd.cdw11 = (uint32_t)((TEST_LBA + submitted) >> 32);
            cmd.cdw12 = 0;  /* 1 block */

            long t0 = now_us();
            int cid = nvme_qpair_submit(io, &cmd);
            if (cid < 0) break;

            submit_time[submitted % MAX_INFLIGHT] = t0;
            cid_to_slot[cid] = submitted % MAX_INFLIGHT;
            submitted++;
        }

        /* Poll for completions */
        int before = completed;
        int n = nvme_qpair_poll(io);

        /* Check which CIDs completed */
        for (int c = 0; c < io->depth && completed < submitted; c++) {
            uint16_t st;
            if (nvme_qpair_cid_done(io, c, &st, NULL)) {
                if (cid_to_slot[c] >= 0) {
                    long t1 = now_us();
                    long lat = t1 - submit_time[cid_to_slot[c]];
                    if (completed < total_cmds)
                        samples[completed].us = lat;
                    completed++;
                    cid_to_slot[c] = -1;

                    /* Reset tracker for reuse */
                    io->tracker[c].status = 0xFFFF;

                    if (st != 0) {
                        log_msg(LOG_ERROR, "  CID=%d failed SF=0x%04X", c, st);
                        return -1;
                    }

                    if (opcode == NVME_IO_READ)
                        dma_invalidate(bufs[c % num_bufs].virt, block_size);
                }
            }
        }

        if (completed == before && n == 0)
            usleep(10);
    }

    long wall_end = now_us();
    long wall_us = wall_end - wall_start;

    /* Override throughput calculation with wall time for accuracy */
    double iops = (double)total_cmds / ((double)wall_us / 1e6);
    double mb_s = iops * block_size / (1024.0 * 1024.0);

    qsort(samples, total_cmds, sizeof(samples[0]), cmp_long);
    long total_lat = 0;
    for (int i = 0; i < total_cmds; i++) total_lat += samples[i].us;

    log_msg(LOG_INFO, "    Latency: min=%ld avg=%ld p50=%ld p99=%ld max=%ld us",
            samples[0].us, total_lat / total_cmds,
            samples[total_cmds / 2].us,
            samples[(int)(total_cmds * 0.99)].us,
            samples[total_cmds - 1].us);
    log_msg(LOG_INFO, "    Throughput: %.0f IOPS, %.2f MB/s (wall=%ld ms)",
            iops, mb_s, wall_us / 1000);

    return 0;
}

int main(int argc, char *argv[]) {
    struct pci_device pci;
    struct nvme_qpair admin, io;
    int rc = 0;

    log_init((argc > 1 && argv[1][0] == '-' && argv[1][1] == 'q')
             ? LOG_INFO : LOG_TRACE, "nvme_m7.log");

    if (pci_find_nvme(&pci) < 0) { rc = 1; goto done; }
    pci_detect_dma_offset(&pci);
    pci_unbind_driver(&pci);
    pci_enable_bus_master(&pci);
    if (pci_map_bar0(&pci) < 0) { rc = 1; goto rebind; }
    mmio_init(pci.bar0);

    uint64_t cap = mmio_read64(NVME_REG_CAP);
    uint32_t dstrd = CAP_DSTRD(cap);

    /* Reset and enable */
    mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);
    while (mmio_read32(NVME_REG_CSTS) & CSTS_RDY) usleep(1000);

    if (nvme_qpair_create(&admin, 0, ADM_DEPTH, dstrd, pci.dma_offset) < 0)
        { rc = 1; goto cleanup; }
    mmio_write32(NVME_REG_AQA, ((ADM_DEPTH - 1) << 16) | (ADM_DEPTH - 1));
    mmio_write64(NVME_REG_ASQ, nvme_qpair_sq_bus(&admin));
    mmio_write64(NVME_REG_ACQ, nvme_qpair_cq_bus(&admin));
    mmio_write32(NVME_REG_CC, CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_IOSQES_64 | CC_IOCQES_16);
    while (!(mmio_read32(NVME_REG_CSTS) & CSTS_RDY)) usleep(1000);

    /* Set Features + Create I/O queue */
    struct nvme_cmd cmd = {0};
    cmd.opcode = NVME_ADMIN_SET_FEATURES;
    cmd.cdw10 = NVME_FEAT_NUM_QUEUES;
    cmd.cdw11 = (7 << 16) | 7;
    nvme_qpair_submit_sync(&admin, &cmd, NULL, 5000);

    if (nvme_qpair_create(&io, 1, IO_DEPTH, dstrd, pci.dma_offset) < 0)
        { rc = 1; goto shutdown; }

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CREATE_IO_CQ;
    cmd.prp1 = nvme_qpair_cq_bus(&io);
    cmd.cdw10 = ((IO_DEPTH - 1) << 16) | 1;
    cmd.cdw11 = 1;
    nvme_qpair_submit_sync(&admin, &cmd, NULL, 5000);

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CREATE_IO_SQ;
    cmd.prp1 = nvme_qpair_sq_bus(&io);
    cmd.cdw10 = ((IO_DEPTH - 1) << 16) | 1;
    cmd.cdw11 = (1 << 16) | 1;
    nvme_qpair_submit_sync(&admin, &cmd, NULL, 5000);

    /* Allocate DMA buffers for I/O (one per max inflight slot) */
    struct dma_buffer bufs[MAX_INFLIGHT];
    for (int i = 0; i < MAX_INFLIGHT; i++) {
        if (dma_alloc(&bufs[i], 512, pci.dma_offset) < 0) { rc = 1; goto shutdown; }
        memset(bufs[i].virt, (uint8_t)(0xA0 + i), 512);
        dma_flush(bufs[i].virt, 512);
    }

    struct latency_sample *samples = calloc(NUM_CMDS, sizeof(struct latency_sample));
    if (!samples) { rc = 1; goto shutdown; }

    /* === Write workloads === */
    int qds[] = {1, 4, 16};
    for (int q = 0; q < 3; q++) {
        log_msg(LOG_INFO, "Write QD=%d (%d commands):", qds[q], NUM_CMDS);
        memset(samples, 0, NUM_CMDS * sizeof(samples[0]));
        if (run_workload(&io, bufs, MAX_INFLIGHT, NVME_IO_WRITE,
                         qds[q], NUM_CMDS, 512, samples) < 0) {
            rc = 1; goto shutdown;
        }
    }

    /* === Read workloads === */
    for (int q = 0; q < 3; q++) {
        log_msg(LOG_INFO, "Read QD=%d (%d commands):", qds[q], NUM_CMDS);
        memset(samples, 0, NUM_CMDS * sizeof(samples[0]));
        if (run_workload(&io, bufs, MAX_INFLIGHT, NVME_IO_READ,
                         qds[q], NUM_CMDS, 512, samples) < 0) {
            rc = 1; goto shutdown;
        }
    }

    free(samples);

shutdown:
    for (int i = 0; i < MAX_INFLIGHT; i++) dma_free(&bufs[i]);
    mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);
    nvme_qpair_destroy(&io);
    nvme_qpair_destroy(&admin);
cleanup:
    pci_unmap_bar0(&pci);
rebind:
    pci_rebind_nvme_driver(&pci);
done:
    log_shutdown();
    return rc;
}
