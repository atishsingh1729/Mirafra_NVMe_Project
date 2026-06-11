/**
 * @file nvme_ctrl.c
 * @brief NVMe controller lifecycle and admin command path.
 */

#include "nvme_ctrl.h"
#include "mmio.h"
#include "dma.h"
#include "log.h"
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── Internal: doorbell helpers ──────────────────────────────── */

static void ring_admin_sq_doorbell(struct nvme_ctrl *ctrl)
{
    uint32_t off = nvme_sq_doorbell_offset(0, ctrl->doorbell_stride >> 2 ? 0 : 0);
    /* Doorbell stride value = 4 << CAP.DSTRD, so DSTRD = 0 for 4-byte stride */
    off = 0x1000; /* Admin SQ is always at doorbell offset 0x1000 for DSTRD=0 */
    mmio_barrier();
    mmio_write32(off, ctrl->admin_sq_tail);
    mmio_barrier();
}

static void ring_admin_cq_doorbell(struct nvme_ctrl *ctrl)
{
    uint32_t off = 0x1000 + ctrl->doorbell_stride; /* CQ0 head DB */
    mmio_barrier();
    mmio_write32(off, ctrl->admin_cq_head);
    mmio_barrier();
}

/* ── Internal: monotonic elapsed time ────────────────────────── */

static long elapsed_ms(struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000 +
           (now.tv_nsec - start->tv_nsec) / 1000000;
}

/* ── Public API ──────────────────────────────────────────────── */

int nvme_ctrl_init(struct nvme_ctrl *ctrl, struct pci_device *pci)
{
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->pci = pci;

    /* Read and cache CAP (Controller Capabilities) */
    ctrl->cap = mmio_read64(NVME_REG_CAP);
    ctrl->max_queue_entries = CAP_MQES(ctrl->cap) + 1;
    ctrl->doorbell_stride = 4 << CAP_DSTRD(ctrl->cap);

    uint8_t to = CAP_TO(ctrl->cap);
    ctrl->timeout_ms = (to > 0) ? (uint32_t)to * 500 : 5000;

    ctrl->admin_cq_phase = 1; /* Phase starts at 1 after reset */

    log_msg(LOG_INFO, "Controller context: MQES=%u  DB_stride=%u  timeout=%u ms",
            ctrl->max_queue_entries, ctrl->doorbell_stride, ctrl->timeout_ms);
    return 0;
}

int nvme_ctrl_reset_and_enable(struct nvme_ctrl *ctrl)
{
    struct timespec t0;

    /* ── Step 1: Disable controller ──────────────────────────── */
    uint32_t cc = mmio_read32(NVME_REG_CC);
    if (cc & CC_EN) {
        log_msg(LOG_INFO, "Disabling controller (CC.EN → 0)...");
        mmio_write32(NVME_REG_CC, cc & ~CC_EN);
    }

    /* Wait for CSTS.RDY = 0 */
    log_msg(LOG_DEBUG, "Waiting for CSTS.RDY = 0...");
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (1) {
        uint32_t csts = mmio_read32(NVME_REG_CSTS);
        if (!(csts & CSTS_RDY)) break;
        if (csts & CSTS_CFS) {
            log_msg(LOG_ERROR, "Controller Fatal Status during disable!");
            return -1;
        }
        if (elapsed_ms(&t0) > (long)ctrl->timeout_ms) {
            log_msg(LOG_ERROR, "Timeout waiting for RDY=0 (%u ms)", ctrl->timeout_ms);
            return -1;
        }
        usleep(1000);
    }
    log_msg(LOG_INFO, "Controller disabled (CSTS.RDY = 0).");

    /* ── Step 2: Allocate admin queue DMA buffers ────────────── */
    size_t sq_size = NVME_ADMIN_Q_DEPTH * sizeof(struct nvme_cmd);   /* 32×64 = 2048 */
    size_t cq_size = NVME_ADMIN_Q_DEPTH * sizeof(struct nvme_cpl);   /* 32×16 = 512  */

    if (dma_alloc(&ctrl->admin_sq, sq_size, ctrl->pci->dma_offset) < 0) {
        log_msg(LOG_ERROR, "Failed to allocate admin SQ DMA buffer");
        return -1;
    }
    if (dma_alloc(&ctrl->admin_cq, cq_size, ctrl->pci->dma_offset) < 0) {
        log_msg(LOG_ERROR, "Failed to allocate admin CQ DMA buffer");
        dma_free(&ctrl->admin_sq);
        return -1;
    }

    log_msg(LOG_INFO, "Admin SQ: virt=%p  phys=0x%llX  bus=0x%llX",
            ctrl->admin_sq.virt,
            (unsigned long long)ctrl->admin_sq.phys,
            (unsigned long long)ctrl->admin_sq.bus);
    log_msg(LOG_INFO, "Admin CQ: virt=%p  phys=0x%llX  bus=0x%llX",
            ctrl->admin_cq.virt,
            (unsigned long long)ctrl->admin_cq.phys,
            (unsigned long long)ctrl->admin_cq.bus);

    /* ── Step 3: Write admin queue config to BAR0 registers ──── */
    uint32_t aqa = ((NVME_ADMIN_Q_DEPTH - 1) << 16) |  /* ACQS (0-based) */
                    (NVME_ADMIN_Q_DEPTH - 1);           /* ASQS (0-based) */
    mmio_write32(NVME_REG_AQA, aqa);
    mmio_write64(NVME_REG_ASQ, ctrl->admin_sq.bus);
    mmio_write64(NVME_REG_ACQ, ctrl->admin_cq.bus);

    /* Reset queue indices */
    ctrl->admin_sq_tail  = 0;
    ctrl->admin_cq_head  = 0;
    ctrl->admin_cq_phase = 1;
    ctrl->cmd_id_counter = 0;

    /* ── Step 4: Enable controller ───────────────────────────── */
    uint32_t new_cc = CC_EN | CC_CSS_NVM | CC_MPS_4K |
                      CC_IOSQES_64 | CC_IOCQES_16;
    log_msg(LOG_INFO, "Enabling controller (CC = 0x%08X)...", new_cc);
    mmio_write32(NVME_REG_CC, new_cc);

    /* Wait for CSTS.RDY = 1 */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (1) {
        uint32_t csts = mmio_read32(NVME_REG_CSTS);
        if (csts & CSTS_RDY) break;
        if (csts & CSTS_CFS) {
            log_msg(LOG_ERROR, "Controller Fatal Status during enable!");
            log_msg(LOG_ERROR, "DMA addresses may be wrong (check dma_offset).");
            return -1;
        }
        if (csts == 0xFFFFFFFF) {
            log_msg(LOG_ERROR, "CSTS=0xFFFFFFFF — PCIe link is down!");
            return -1;
        }
        if (elapsed_ms(&t0) > (long)ctrl->timeout_ms) {
            log_msg(LOG_ERROR, "Timeout waiting for RDY=1 (%u ms)", ctrl->timeout_ms);
            return -1;
        }
        usleep(1000);
    }
    log_msg(LOG_INFO, "Controller ENABLED and READY.");
    return 0;
}

void nvme_ctrl_shutdown(struct nvme_ctrl *ctrl)
{
    /* Disable controller */
    uint32_t cc = mmio_read32(NVME_REG_CC);
    if (cc & CC_EN) {
        mmio_write32(NVME_REG_CC, cc & ~CC_EN);
        log_msg(LOG_INFO, "Controller disabled.");
    }

    /* Free admin queue buffers */
    dma_free(&ctrl->admin_sq);
    dma_free(&ctrl->admin_cq);
    log_msg(LOG_DEBUG, "Admin queue buffers freed.");
}

int nvme_admin_submit_sync(struct nvme_ctrl *ctrl,
                           struct nvme_cmd *cmd,
                           uint32_t *result_out,
                           int timeout_ms)
{
    /* Assign CID (Command Identifier) */
    cmd->cid = ctrl->cmd_id_counter++;

    /* Write command to SQ at tail position */
    struct nvme_cmd *sq = (struct nvme_cmd *)ctrl->admin_sq.virt;
    memcpy(&sq[ctrl->admin_sq_tail], cmd, sizeof(*cmd));

    /* Flush SQ entry from cache to RAM so controller can DMA-read it */
    dma_flush(&sq[ctrl->admin_sq_tail], sizeof(*cmd));

    /* Advance tail and ring doorbell */
    ctrl->admin_sq_tail = (ctrl->admin_sq_tail + 1) % NVME_ADMIN_Q_DEPTH;
    ring_admin_sq_doorbell(ctrl);

    /* Poll CQ for completion */
    struct nvme_cpl *cq = (struct nvme_cpl *)ctrl->admin_cq.virt;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (1) {
        /* Invalidate CQ entry cache line to read fresh DMA data */
        dma_invalidate(&cq[ctrl->admin_cq_head], sizeof(struct nvme_cpl));

        struct nvme_cpl *entry = &cq[ctrl->admin_cq_head];
        uint16_t status_raw = entry->status;
        uint8_t phase = CQE_PHASE(status_raw);

        if (phase == ctrl->admin_cq_phase) {
            /* Got a completion! */
            uint16_t sf = CQE_STATUS(status_raw);

            if (result_out) *result_out = entry->result;

            /* Advance CQ head, toggle phase on wrap */
            ctrl->admin_cq_head++;
            if (ctrl->admin_cq_head >= NVME_ADMIN_Q_DEPTH) {
                ctrl->admin_cq_head = 0;
                ctrl->admin_cq_phase ^= 1;
            }
            ring_admin_cq_doorbell(ctrl);

            if (sf != 0) {
                log_msg(LOG_WARN, "Admin cmd 0x%02X CID=%u failed: SF=0x%04X (SC=0x%02X SCT=%u)",
                        cmd->opcode, cmd->cid, sf, CQE_SC(sf), CQE_SCT(sf));
            }
            return (int)sf;
        }

        if (elapsed_ms(&t0) > timeout_ms) {
            uint32_t csts = mmio_read32(NVME_REG_CSTS);
            log_msg(LOG_ERROR, "Admin CQ poll timeout (%d ms)  CSTS=0x%08X", timeout_ms, csts);
            return -1;
        }
        usleep(100);
    }
}

int nvme_set_num_queues(struct nvme_ctrl *ctrl,
                        uint16_t nsq, uint16_t ncq,
                        uint16_t *nsq_out, uint16_t *ncq_out)
{
    struct nvme_cmd cmd = {0};
    cmd.opcode = NVME_ADMIN_SET_FEATURES;
    cmd.cdw10  = NVME_FEAT_NUM_QUEUES;
    cmd.cdw11  = (((uint32_t)(ncq - 1)) << 16) | (nsq - 1); /* 0-based */

    uint32_t result = 0;
    int sf = nvme_admin_submit_sync(ctrl, &cmd, &result, 5000);
    if (sf < 0) return -1;
    if (sf != 0) return sf;

    if (nsq_out) *nsq_out = (result & 0xFFFF) + 1;
    if (ncq_out) *ncq_out = ((result >> 16) & 0xFFFF) + 1;

    log_msg(LOG_INFO, "Set Features NumQueues: allocated %u SQs, %u CQs",
            (result & 0xFFFF) + 1, ((result >> 16) & 0xFFFF) + 1);
    return 0;
}
