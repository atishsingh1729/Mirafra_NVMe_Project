/*
 * nvme_queue.c — Generic NVMe queue pair management.
 */

#include "nvme_queue.h"
#include "mmio.h"
#include "log.h"
#include <string.h>
#include <time.h>
#include <unistd.h>

int nvme_qpair_create(struct nvme_qpair *qp, uint16_t qid,
                      uint16_t depth, uint32_t dstrd,
                      uint64_t dma_offset)
{
    memset(qp, 0, sizeof(*qp));
    qp->qid   = qid;
    qp->depth  = depth;

    if (depth > NVME_MAX_Q_DEPTH) {
        log_msg(LOG_ERROR, "Queue depth %u exceeds max %u", depth, NVME_MAX_Q_DEPTH);
        return -1;
    }

    /* Allocate SQ and CQ DMA buffers */
    size_t sq_bytes = depth * sizeof(struct nvme_cmd);
    size_t cq_bytes = depth * sizeof(struct nvme_cpl);

    if (dma_alloc(&qp->sq, sq_bytes, dma_offset) < 0) return -1;
    if (dma_alloc(&qp->cq, cq_bytes, dma_offset) < 0) {
        dma_free(&qp->sq);
        return -1;
    }

    /* Compute doorbell offsets in BAR0 */
    uint32_t stride = 4 << dstrd;
    qp->sq_db_off = 0x1000 + (2 * qid) * stride;       /* SQ tail doorbell */
    qp->cq_db_off = 0x1000 + (2 * qid + 1) * stride;   /* CQ head doorbell */

    /* Initial state */
    qp->sq_tail  = 0;
    qp->cq_head  = 0;
    qp->cq_phase = 1;
    qp->cid_next = 0;
    qp->cmds_in_flight = 0;

    log_msg(LOG_DEBUG, "QP%u created: depth=%u  SQ_DB=0x%X  CQ_DB=0x%X  SQ_bus=0x%llX  CQ_bus=0x%llX",
            qid, depth, qp->sq_db_off, qp->cq_db_off,
            (unsigned long long)qp->sq.bus, (unsigned long long)qp->cq.bus);
    return 0;
}

void nvme_qpair_destroy(struct nvme_qpair *qp)
{
    dma_free(&qp->sq);
    dma_free(&qp->cq);
    log_msg(LOG_DEBUG, "QP%u destroyed", qp->qid);
    memset(qp, 0, sizeof(*qp));
}

int nvme_qpair_submit(struct nvme_qpair *qp, struct nvme_cmd *cmd)
{
    /* Check if queue is full (one slot always reserved to distinguish full/empty) */
    uint16_t next_tail = (qp->sq_tail + 1) % qp->depth;
    if (qp->cmds_in_flight >= qp->depth - 1) {
        log_msg(LOG_WARN, "QP%u: SQ full (%u in flight)", qp->qid, qp->cmds_in_flight);
        return -1;
    }

    /* Assign CID and mark slot busy */
    uint16_t cid = qp->cid_next % qp->depth;
    qp->cid_next++;
    cmd->cid = cid;

    qp->tracker[cid].busy   = 1;
    qp->tracker[cid].status = 0xFFFF;  /* sentinel: not yet completed */
    qp->tracker[cid].result = 0;

    /* Copy command into SQ ring at tail position */
    struct nvme_cmd *sq = (struct nvme_cmd *)qp->sq.virt;
    memcpy(&sq[qp->sq_tail], cmd, sizeof(*cmd));

    /* Flush SQ entry from cache to RAM */
    dma_flush(&sq[qp->sq_tail], sizeof(*cmd));

    /* Advance tail (circular) */
    qp->sq_tail = next_tail;
    qp->cmds_in_flight++;

    /* Ring the SQ tail doorbell */
    mmio_barrier();
    mmio_write32(qp->sq_db_off, qp->sq_tail);
    mmio_barrier();

    log_msg(LOG_DEBUG, "QP%u: submitted CID=%u  opcode=0x%02X  tail=%u  in_flight=%u",
            qp->qid, cid, cmd->opcode, qp->sq_tail, qp->cmds_in_flight);
    return (int)cid;
}

int nvme_qpair_poll(struct nvme_qpair *qp)
{
    struct nvme_cpl *cq = (struct nvme_cpl *)qp->cq.virt;
    int processed = 0;

    while (1) {
        /* Invalidate the CQ entry cache line to see fresh DMA data */
        dma_invalidate(&cq[qp->cq_head], sizeof(struct nvme_cpl));

        uint16_t status_raw = cq[qp->cq_head].status;
        uint8_t phase = CQE_PHASE(status_raw);

        /* No new completion if phase doesn't match */
        if (phase != qp->cq_phase)
            break;

        /* Extract fields from the completion entry */
        uint16_t cid    = cq[qp->cq_head].cid;
        uint16_t sf     = CQE_STATUS(status_raw);
        uint32_t result = cq[qp->cq_head].result;

        /* Update tracker for this CID */
        if (cid < qp->depth && qp->tracker[cid].busy) {
            qp->tracker[cid].busy   = 0;
            qp->tracker[cid].status = sf;
            qp->tracker[cid].result = result;
            if (qp->cmds_in_flight > 0)
                qp->cmds_in_flight--;
        } else {
            log_msg(LOG_WARN, "QP%u: unexpected CID=%u (not tracked)", qp->qid, cid);
        }

        log_msg(LOG_DEBUG, "QP%u: completed CID=%u  SF=0x%04X  in_flight=%u",
                qp->qid, cid, sf, qp->cmds_in_flight);

        /* Advance CQ head, toggle phase on wrap */
        qp->cq_head++;
        if (qp->cq_head >= qp->depth) {
            qp->cq_head = 0;
            qp->cq_phase ^= 1;
        }
        processed++;
    }

    /* If we processed any completions, ring the CQ head doorbell */
    if (processed > 0) {
        mmio_barrier();
        mmio_write32(qp->cq_db_off, qp->cq_head);
        mmio_barrier();
    }

    return processed;
}

int nvme_qpair_submit_sync(struct nvme_qpair *qp, struct nvme_cmd *cmd,
                           uint32_t *result_out, int timeout_ms)
{
    int cid = nvme_qpair_submit(qp, cmd);
    if (cid < 0) return -1;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (1) {
        nvme_qpair_poll(qp);

        uint16_t sf;
        uint32_t res;
        if (nvme_qpair_cid_done(qp, (uint16_t)cid, &sf, &res)) {
            if (result_out) *result_out = res;
            return (int)sf;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - t0.tv_sec) * 1000 +
                       (now.tv_nsec - t0.tv_nsec) / 1000000;
        if (elapsed > timeout_ms) {
            log_msg(LOG_ERROR, "QP%u: CID=%u timeout (%d ms)", qp->qid, cid, timeout_ms);
            return -1;
        }
        usleep(100);
    }
}

int nvme_qpair_cid_done(struct nvme_qpair *qp, uint16_t cid,
                        uint16_t *status_out, uint32_t *result_out)
{
    if (cid >= qp->depth) return 0;
    if (qp->tracker[cid].busy) return 0;             /* still in flight */
    if (qp->tracker[cid].status == 0xFFFF) return 0;  /* never submitted */

    if (status_out) *status_out = qp->tracker[cid].status;
    if (result_out) *result_out = qp->tracker[cid].result;
    return 1;
}
