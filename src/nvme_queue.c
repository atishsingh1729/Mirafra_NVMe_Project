#include "nvme_queue.h"
#include "mmio.h"
#include "log.h"
#include <string.h>
#include <time.h>
#include <unistd.h>

int nvme_qpair_create(struct nvme_qpair *qp, uint16_t qid, uint16_t depth,
                      uint32_t dstrd, uint64_t dma_offset) {
    memset(qp, 0, sizeof(*qp));
    qp->qid = qid; qp->depth = depth;
    if (depth > NVME_MAX_Q_DEPTH) return -1;

    if (dma_alloc(&qp->sq, depth * sizeof(struct nvme_cmd), dma_offset) < 0) return -1;
    if (dma_alloc(&qp->cq, depth * sizeof(struct nvme_cpl), dma_offset) < 0) {
        dma_free(&qp->sq); return -1;
    }

    uint32_t stride = 4 << dstrd;
    qp->sq_db_off = 0x1000 + (2 * qid) * stride;
    qp->cq_db_off = 0x1000 + (2 * qid + 1) * stride;
    qp->cq_phase = 1;
    return 0;
}

void nvme_qpair_destroy(struct nvme_qpair *qp) {
    dma_free(&qp->sq); dma_free(&qp->cq);
    memset(qp, 0, sizeof(*qp));
}

int nvme_qpair_submit(struct nvme_qpair *qp, struct nvme_cmd *cmd) {
    if (qp->cmds_in_flight >= qp->depth - 1) return -1;

    uint16_t cid = qp->cid_next % qp->depth;
    qp->cid_next++;
    cmd->cid = cid;
    qp->tracker[cid].busy = 1;
    qp->tracker[cid].status = 0xFFFF;

    struct nvme_cmd *sq = (struct nvme_cmd *)qp->sq.virt;
    memcpy(&sq[qp->sq_tail], cmd, sizeof(*cmd));
    dma_flush(&sq[qp->sq_tail], sizeof(*cmd));

    qp->sq_tail = (qp->sq_tail + 1) % qp->depth;
    qp->cmds_in_flight++;

    mmio_barrier(); mmio_write32(qp->sq_db_off, qp->sq_tail); mmio_barrier();
    return (int)cid;
}

int nvme_qpair_poll(struct nvme_qpair *qp) {
    struct nvme_cpl *cq = (struct nvme_cpl *)qp->cq.virt;
    int n = 0;
    while (1) {
        dma_invalidate(&cq[qp->cq_head], sizeof(struct nvme_cpl));
        uint16_t raw = cq[qp->cq_head].status;
        if (CQE_PHASE(raw) != qp->cq_phase) break;

        uint16_t cid = cq[qp->cq_head].cid;
        uint16_t sf = CQE_STATUS(raw);
        if (cid < qp->depth && qp->tracker[cid].busy) {
            qp->tracker[cid].busy = 0;
            qp->tracker[cid].status = sf;
            qp->tracker[cid].result = cq[qp->cq_head].result;
            if (qp->cmds_in_flight > 0) qp->cmds_in_flight--;
        }
        qp->cq_head++;
        if (qp->cq_head >= qp->depth) { qp->cq_head = 0; qp->cq_phase ^= 1; }
        n++;
    }
    if (n > 0) { mmio_barrier(); mmio_write32(qp->cq_db_off, qp->cq_head); mmio_barrier(); }
    return n;
}

int nvme_qpair_submit_sync(struct nvme_qpair *qp, struct nvme_cmd *cmd,
                           uint32_t *result_out, int timeout_ms) {
    int cid = nvme_qpair_submit(qp, cmd);
    if (cid < 0) return -1;

    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    while (1) {
        nvme_qpair_poll(qp);
        uint16_t sf; uint32_t res;
        if (nvme_qpair_cid_done(qp, (uint16_t)cid, &sf, &res)) {
            if (result_out) *result_out = res;
            return (int)sf;
        }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms > timeout_ms) return -1;
        usleep(100);
    }
}

int nvme_qpair_cid_done(struct nvme_qpair *qp, uint16_t cid,
                        uint16_t *status_out, uint32_t *result_out) {
    if (cid >= qp->depth || qp->tracker[cid].busy || qp->tracker[cid].status == 0xFFFF) return 0;
    if (status_out) *status_out = qp->tracker[cid].status;
    if (result_out) *result_out = qp->tracker[cid].result;
    return 1;
}
