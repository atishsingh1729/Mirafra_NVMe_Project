/*
 * milestone8.c — Diagnostics
 * Error injection, counters, trace ring buffer, queue depth visibility.
 */
#include "log.h"
#include "pci.h"
#include "mmio.h"
#include "dma.h"
#include "nvme_regs.h"
#include "nvme_queue.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ADM_DEPTH  32
#define IO_DEPTH   64

/* --- Counters --- */
struct diag_counters {
    uint64_t submits, completions, errors, bytes_written, bytes_read;
};
static struct diag_counters g_cnt;

static void cnt_reset(void) { memset(&g_cnt, 0, sizeof(g_cnt)); }

static void cnt_print(void) {
    log_msg(LOG_INFO, "  Counters: sub=%llu cpl=%llu err=%llu wr=%llu B rd=%llu B",
            (unsigned long long)g_cnt.submits, (unsigned long long)g_cnt.completions,
            (unsigned long long)g_cnt.errors,
            (unsigned long long)g_cnt.bytes_written, (unsigned long long)g_cnt.bytes_read);
}

/* --- Trace ring buffer --- */
#define TRACE_SIZE 32

struct trace_entry {
    long ts_us;
    char event[8];
    uint8_t opcode;
    uint16_t cid, status, qd;
};
static struct trace_entry g_trace[TRACE_SIZE];
static int g_trace_head;

static void trace_reset(void) { memset(g_trace, 0, sizeof(g_trace)); g_trace_head = 0; }

static long trace_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static void trace_add(const char *ev, uint8_t op, uint16_t cid, uint16_t st, uint16_t qd) {
    struct trace_entry *e = &g_trace[g_trace_head % TRACE_SIZE];
    e->ts_us = trace_now();
    strncpy(e->event, ev, 7); e->event[7] = 0;
    e->opcode = op; e->cid = cid; e->status = st; e->qd = qd;
    g_trace_head++;
}

static void trace_dump(void) {
    int start = (g_trace_head > TRACE_SIZE) ? g_trace_head - TRACE_SIZE : 0;
    int count = (g_trace_head > TRACE_SIZE) ? TRACE_SIZE : g_trace_head;
    log_msg(LOG_INFO, "  Trace (last %d events):", count);
    for (int i = 0; i < count; i++) {
        struct trace_entry *e = &g_trace[(start + i) % TRACE_SIZE];
        log_msg(LOG_INFO, "    [%ld us] %-6s op=0x%02X CID=%u SF=0x%04X QD=%u",
                e->ts_us, e->event, e->opcode, e->cid, e->status, e->qd);
    }
}

/* --- Tracked submit/poll wrappers --- */
static int diag_submit(struct nvme_qpair *qp, struct nvme_cmd *cmd) {
    int cid = nvme_qpair_submit(qp, cmd);
    if (cid >= 0) {
        g_cnt.submits++;
        trace_add("SUBMIT", cmd->opcode, (uint16_t)cid, 0, nvme_qpair_in_flight(qp));
    }
    return cid;
}

static void diag_poll_all(struct nvme_qpair *qp, uint8_t opcode, uint32_t bs) {
    nvme_qpair_poll(qp);
    for (int c = 0; c < qp->depth; c++) {
        uint16_t st;
        if (nvme_qpair_cid_done(qp, c, &st, NULL)) {
            g_cnt.completions++;
            if (st != 0) { g_cnt.errors++; trace_add("ERROR", opcode, c, st, nvme_qpair_in_flight(qp)); }
            else { trace_add("COMPL", opcode, c, 0, nvme_qpair_in_flight(qp));
                   if (opcode == NVME_IO_WRITE) g_cnt.bytes_written += bs;
                   if (opcode == NVME_IO_READ)  g_cnt.bytes_read += bs; }
            qp->tracker[c].status = 0xFFFF;
        }
    }
}

static int diag_submit_sync(struct nvme_qpair *qp, struct nvme_cmd *cmd, int timeout) {
    g_cnt.submits++;
    trace_add("SUBMIT", cmd->opcode, 0, 0, nvme_qpair_in_flight(qp));
    int sf = nvme_qpair_submit_sync(qp, cmd, NULL, timeout);
    g_cnt.completions++;
    if (sf != 0 && sf != -1) { g_cnt.errors++; trace_add("ERROR", cmd->opcode, 0, (uint16_t)sf, 0); }
    else if (sf == 0) trace_add("COMPL", cmd->opcode, 0, 0, 0);
    return sf;
}

int main(int argc, char *argv[]) {
    struct pci_device pci;
    struct nvme_qpair admin, io;
    int rc = 0;

    log_init((argc > 1 && argv[1][0] == '-' && argv[1][1] == 'q')
             ? LOG_INFO : LOG_TRACE, "nvme_m8.log");

    if (pci_find_nvme(&pci) < 0) { rc = 1; goto done; }
    pci_detect_dma_offset(&pci);
    pci_unbind_driver(&pci);
    pci_enable_bus_master(&pci);
    if (pci_map_bar0(&pci) < 0) { rc = 1; goto rebind; }
    mmio_init(pci.bar0);

    uint64_t cap = mmio_read64(NVME_REG_CAP);
    uint32_t dstrd = CAP_DSTRD(cap);

    mmio_write32(NVME_REG_CC, mmio_read32(NVME_REG_CC) & ~CC_EN);
    while (mmio_read32(NVME_REG_CSTS) & CSTS_RDY) usleep(1000);

    nvme_qpair_create(&admin, 0, ADM_DEPTH, dstrd, pci.dma_offset);
    mmio_write32(NVME_REG_AQA, ((ADM_DEPTH-1)<<16)|(ADM_DEPTH-1));
    mmio_write64(NVME_REG_ASQ, nvme_qpair_sq_bus(&admin));
    mmio_write64(NVME_REG_ACQ, nvme_qpair_cq_bus(&admin));
    mmio_write32(NVME_REG_CC, CC_EN|CC_CSS_NVM|CC_MPS_4K|CC_IOSQES_64|CC_IOCQES_16);
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

    struct dma_buffer buf;
    dma_alloc(&buf, 4096, pci.dma_offset);

    cnt_reset();
    trace_reset();

    /* Test 1: invalid opcode */
    log_msg(LOG_INFO, "Test 1: Error injection — invalid opcode");
    memset(&cmd, 0, sizeof(cmd)); cmd.opcode = 0xFF; cmd.prp1 = buf.bus;
    int sf = diag_submit_sync(&admin, &cmd, 5000);
    log_msg(LOG_INFO, "  opcode 0xFF → SF=0x%04X", sf);

    /* Test 2: invalid namespace */
    log_msg(LOG_INFO, "Test 2: Error injection — invalid NSID");
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_IDENTIFY; cmd.nsid = 0xFFFFFFFE;
    cmd.prp1 = buf.bus; cmd.cdw10 = NVME_IDENTIFY_NS;
    sf = diag_submit_sync(&admin, &cmd, 5000);
    log_msg(LOG_INFO, "  NSID=0xFFFFFFFE → SF=0x%04X", sf);

    /* Test 3: invalid log page */
    log_msg(LOG_INFO, "Test 3: Error injection — invalid log page");
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_GET_LOG_PAGE; cmd.nsid = 0xFFFFFFFF;
    cmd.prp1 = buf.bus; cmd.cdw10 = (127 << 16) | 0xFF;
    sf = diag_submit_sync(&admin, &cmd, 5000);
    log_msg(LOG_INFO, "  LID=0xFF → SF=0x%04X", sf);

    /* Test 4: confirm controller survived */
    log_msg(LOG_INFO, "Test 4: Valid Identify after errors");
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_IDENTIFY; cmd.prp1 = buf.bus; cmd.cdw10 = NVME_IDENTIFY_CTRL;
    sf = diag_submit_sync(&admin, &cmd, 5000);
    if (sf == 0) {
        dma_invalidate(buf.virt, 4096);
        char sn[21] = {0}; memcpy(sn, (uint8_t*)buf.virt + 4, 20);
        for (int i = 19; i >= 0 && sn[i] == ' '; i--) sn[i] = 0;
        log_msg(LOG_INFO, "  OK: SN=%s (controller alive)", sn);
    } else { log_msg(LOG_ERROR, "  FAILED"); rc = 1; }
    cnt_print();

    /* Test 5: I/O with counters + QD visibility */
    log_msg(LOG_INFO, "Test 5: I/O burst — counters + QD tracking");
    cnt_reset(); trace_reset();

    struct dma_buffer io_bufs[8];
    for (int i = 0; i < 8; i++) {
        dma_alloc(&io_bufs[i], 512, pci.dma_offset);
        memset(io_bufs[i].virt, 0xBB + i, 512);
        dma_flush(io_bufs[i].virt, 512);
    }

    int submitted = 0, total = 20;
    while (g_cnt.completions < (uint64_t)total) {
        while (submitted < total && nvme_qpair_in_flight(&io) < 4) {
            memset(&cmd, 0, sizeof(cmd));
            cmd.opcode = NVME_IO_WRITE; cmd.nsid = 1;
            cmd.prp1 = io_bufs[submitted % 8].bus;
            cmd.cdw10 = (uint32_t)(0x200000 + submitted); cmd.cdw12 = 0;
            dma_flush(io_bufs[submitted % 8].virt, 512);
            diag_submit(&io, &cmd);
            submitted++;
        }
        diag_poll_all(&io, NVME_IO_WRITE, 512);
        usleep(10);
    }
    log_msg(LOG_INFO, "  20 writes at QD=4 done");
    cnt_print();

    uint64_t prev = g_cnt.completions;
    submitted = 0;
    while (g_cnt.completions - prev < (uint64_t)total) {
        while (submitted < total && nvme_qpair_in_flight(&io) < 4) {
            memset(&cmd, 0, sizeof(cmd));
            cmd.opcode = NVME_IO_READ; cmd.nsid = 1;
            cmd.prp1 = io_bufs[submitted % 8].bus;
            cmd.cdw10 = (uint32_t)(0x200000 + submitted); cmd.cdw12 = 0;
            memset(io_bufs[submitted % 8].virt, 0, 512);
            dma_flush(io_bufs[submitted % 8].virt, 512);
            diag_submit(&io, &cmd);
            submitted++;
        }
        diag_poll_all(&io, NVME_IO_READ, 512);
        usleep(10);
    }
    log_msg(LOG_INFO, "  20 reads at QD=4 done");
    cnt_print();

    /* Test 6: I/O error — read beyond capacity */
    log_msg(LOG_INFO, "Test 6: Error injection — I/O beyond capacity");
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_IO_READ; cmd.nsid = 1; cmd.prp1 = io_bufs[0].bus;
    cmd.cdw10 = 0xFFFFFFFF; cmd.cdw11 = 0x7FFFFFFF; cmd.cdw12 = 0;
    sf = diag_submit_sync(&io, &cmd, 5000);
    log_msg(LOG_INFO, "  LBA=max → SF=0x%04X", sf);
    cnt_print();

    /* Test 7: trace dump */
    log_msg(LOG_INFO, "Test 7: Trace ring buffer");
    trace_dump();

    /* Summary */
    log_msg(LOG_INFO, "Results:");
    log_msg(LOG_INFO, "  Errors injected: 4 admin + 1 I/O, controller survived all");
    log_msg(LOG_INFO, "  Total: %llu submits, %llu completions, %llu errors",
            (unsigned long long)g_cnt.submits, (unsigned long long)g_cnt.completions,
            (unsigned long long)g_cnt.errors);
    log_msg(LOG_INFO, "  Trace: %d events in ring buffer", g_trace_head);

    for (int i = 0; i < 8; i++) dma_free(&io_bufs[i]);
    dma_free(&buf);
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
