#include "nvme_ctrl.h"
#include "mmio.h"
#include "dma.h"
#include "log.h"
#include <string.h>
#include <time.h>
#include <unistd.h>

static long elapsed_ms(struct timespec *t0) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t0->tv_sec) * 1000 + (now.tv_nsec - t0->tv_nsec) / 1000000;
}

int nvme_ctrl_init(struct nvme_ctrl *ctrl, struct pci_device *pci) {
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->pci = pci;
    ctrl->cap = mmio_read64(NVME_REG_CAP);
    ctrl->max_queue_entries = CAP_MQES(ctrl->cap) + 1;
    ctrl->doorbell_stride = 4 << CAP_DSTRD(ctrl->cap);
    uint8_t to = CAP_TO(ctrl->cap);
    ctrl->timeout_ms = to ? (uint32_t)to * 500 : 5000;
    ctrl->admin_cq_phase = 1;
    return 0;
}

int nvme_ctrl_reset_and_enable(struct nvme_ctrl *ctrl) {
    struct timespec t0;

    /* Disable */
    uint32_t cc = mmio_read32(NVME_REG_CC);
    if (cc & CC_EN) mmio_write32(NVME_REG_CC, cc & ~CC_EN);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (mmio_read32(NVME_REG_CSTS) & CSTS_RDY) {
        if (mmio_read32(NVME_REG_CSTS) & CSTS_CFS) return -1;
        if (elapsed_ms(&t0) > (long)ctrl->timeout_ms) return -1;
        usleep(1000);
    }

    /* Allocate admin queues */
    if (dma_alloc(&ctrl->admin_sq, NVME_ADMIN_Q_DEPTH * sizeof(struct nvme_cmd), ctrl->pci->dma_offset) < 0) return -1;
    if (dma_alloc(&ctrl->admin_cq, NVME_ADMIN_Q_DEPTH * sizeof(struct nvme_cpl), ctrl->pci->dma_offset) < 0) {
        dma_free(&ctrl->admin_sq); return -1;
    }

    /* Configure admin queue registers */
    mmio_write32(NVME_REG_AQA, ((NVME_ADMIN_Q_DEPTH - 1) << 16) | (NVME_ADMIN_Q_DEPTH - 1));
    mmio_write64(NVME_REG_ASQ, ctrl->admin_sq.bus);
    mmio_write64(NVME_REG_ACQ, ctrl->admin_cq.bus);

    ctrl->admin_sq_tail = 0; ctrl->admin_cq_head = 0;
    ctrl->admin_cq_phase = 1; ctrl->cmd_id_counter = 0;

    /* Enable */
    mmio_write32(NVME_REG_CC, CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_IOSQES_64 | CC_IOCQES_16);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!(mmio_read32(NVME_REG_CSTS) & CSTS_RDY)) {
        uint32_t csts = mmio_read32(NVME_REG_CSTS);
        if (csts & CSTS_CFS || csts == 0xFFFFFFFF) return -1;
        if (elapsed_ms(&t0) > (long)ctrl->timeout_ms) return -1;
        usleep(1000);
    }
    return 0;
}

void nvme_ctrl_shutdown(struct nvme_ctrl *ctrl) {
    uint32_t cc = mmio_read32(NVME_REG_CC);
    if (cc & CC_EN) mmio_write32(NVME_REG_CC, cc & ~CC_EN);
    dma_free(&ctrl->admin_sq);
    dma_free(&ctrl->admin_cq);
}

int nvme_admin_submit_sync(struct nvme_ctrl *ctrl, struct nvme_cmd *cmd,
                           uint32_t *result_out, int timeout_ms) {
    cmd->cid = ctrl->cmd_id_counter++;
    struct nvme_cmd *sq = (struct nvme_cmd *)ctrl->admin_sq.virt;
    memcpy(&sq[ctrl->admin_sq_tail], cmd, sizeof(*cmd));
    dma_flush(&sq[ctrl->admin_sq_tail], sizeof(*cmd));

    ctrl->admin_sq_tail = (ctrl->admin_sq_tail + 1) % NVME_ADMIN_Q_DEPTH;
    mmio_barrier(); mmio_write32(0x1000, ctrl->admin_sq_tail); mmio_barrier();

    struct nvme_cpl *cq = (struct nvme_cpl *)ctrl->admin_cq.virt;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);

    while (1) {
        dma_invalidate(&cq[ctrl->admin_cq_head], sizeof(struct nvme_cpl));
        uint16_t raw = cq[ctrl->admin_cq_head].status;
        if (CQE_PHASE(raw) == ctrl->admin_cq_phase) {
            uint16_t sf = CQE_STATUS(raw);
            if (result_out) *result_out = cq[ctrl->admin_cq_head].result;
            ctrl->admin_cq_head++;
            if (ctrl->admin_cq_head >= NVME_ADMIN_Q_DEPTH) { ctrl->admin_cq_head = 0; ctrl->admin_cq_phase ^= 1; }
            mmio_barrier(); mmio_write32(0x1000 + ctrl->doorbell_stride, ctrl->admin_cq_head); mmio_barrier();
            return (int)sf;
        }
        if (elapsed_ms(&t0) > timeout_ms) return -1;
        usleep(100);
    }
}

int nvme_set_num_queues(struct nvme_ctrl *ctrl, uint16_t nsq, uint16_t ncq,
                        uint16_t *nsq_out, uint16_t *ncq_out) {
    struct nvme_cmd cmd = {0};
    cmd.opcode = NVME_ADMIN_SET_FEATURES;
    cmd.cdw10 = NVME_FEAT_NUM_QUEUES;
    cmd.cdw11 = ((uint32_t)(ncq - 1) << 16) | (nsq - 1);
    uint32_t res = 0;
    int sf = nvme_admin_submit_sync(ctrl, &cmd, &res, 5000);
    if (sf < 0 || sf != 0) return sf < 0 ? -1 : sf;
    if (nsq_out) *nsq_out = (res & 0xFFFF) + 1;
    if (ncq_out) *ncq_out = ((res >> 16) & 0xFFFF) + 1;
    return 0;
}
