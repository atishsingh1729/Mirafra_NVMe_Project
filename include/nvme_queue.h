#ifndef NVME_QUEUE_H
#define NVME_QUEUE_H
#include "dma.h"
#include "nvme_regs.h"

#define NVME_MAX_Q_DEPTH 256

struct nvme_cmd_tracker {
    uint8_t busy; uint16_t status; uint32_t result;
};

struct nvme_qpair {
    uint16_t qid, depth;
    struct dma_buffer sq, cq;
    uint16_t sq_tail;
    uint32_t sq_db_off;
    uint16_t cq_head;
    uint8_t  cq_phase;
    uint32_t cq_db_off;
    uint16_t cid_next, cmds_in_flight;
    struct nvme_cmd_tracker tracker[NVME_MAX_Q_DEPTH];
};

int  nvme_qpair_create(struct nvme_qpair *qp, uint16_t qid, uint16_t depth,
                       uint32_t dstrd, uint64_t dma_offset);
void nvme_qpair_destroy(struct nvme_qpair *qp);
int  nvme_qpair_submit(struct nvme_qpair *qp, struct nvme_cmd *cmd);
int  nvme_qpair_poll(struct nvme_qpair *qp);
int  nvme_qpair_submit_sync(struct nvme_qpair *qp, struct nvme_cmd *cmd,
                            uint32_t *result_out, int timeout_ms);
int  nvme_qpair_cid_done(struct nvme_qpair *qp, uint16_t cid,
                         uint16_t *status_out, uint32_t *result_out);

static inline uint16_t nvme_qpair_in_flight(const struct nvme_qpair *qp) {
    return qp->cmds_in_flight;
}
static inline uint64_t nvme_qpair_sq_bus(const struct nvme_qpair *qp) { return qp->sq.bus; }
static inline uint64_t nvme_qpair_cq_bus(const struct nvme_qpair *qp) { return qp->cq.bus; }

#endif
