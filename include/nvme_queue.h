/*
 * nvme_queue.h — Generic NVMe queue pair (SQ + CQ) with CID tracking.
 *
 * Manages ring buffers, doorbells, phase bits, and a per-CID
 * completion lookup table. Works for both admin and I/O queues.
 */

#ifndef NVME_QUEUE_H
#define NVME_QUEUE_H

#include "dma.h"
#include "nvme_regs.h"
#include <stdint.h>

#define NVME_MAX_Q_DEPTH    256  /* max entries we support per queue */

/* Completion status for a tracked command */
struct nvme_cmd_tracker {
    uint8_t  busy;          /* 1 = command in flight, 0 = slot free     */
    uint16_t status;        /* NVMe status field (0 = success)          */
    uint32_t result;        /* CQE DW0 (command-specific result)        */
};

/* A single SQ+CQ queue pair with full ring buffer state */
struct nvme_qpair {
    /* Identity */
    uint16_t qid;           /* Queue ID (0 = admin, 1+ = I/O)          */
    uint16_t depth;         /* Number of entries in SQ and CQ           */

    /* Submission Queue */
    struct dma_buffer sq;   /* SQ DMA buffer                            */
    uint16_t sq_tail;       /* Next slot to write a command              */
    uint32_t sq_db_off;     /* BAR0 offset of SQ tail doorbell          */

    /* Completion Queue */
    struct dma_buffer cq;   /* CQ DMA buffer                            */
    uint16_t cq_head;       /* Next slot to read a completion            */
    uint8_t  cq_phase;      /* Expected phase bit (starts at 1)         */
    uint32_t cq_db_off;     /* BAR0 offset of CQ head doorbell          */

    /* CID tracking table — one slot per possible in-flight command */
    uint16_t cid_next;      /* Next CID to assign                       */
    uint16_t cmds_in_flight;/* Count of commands awaiting completion     */
    struct nvme_cmd_tracker tracker[NVME_MAX_Q_DEPTH];
};

/*
 * Create a queue pair: allocate SQ/CQ DMA buffers, compute
 * doorbell offsets, initialise ring state.
 *
 *   qid       — 0 for admin, 1+ for I/O
 *   depth     — number of entries (must be <= NVME_MAX_Q_DEPTH)
 *   dstrd     — CAP.DSTRD value (doorbell stride field, not bytes)
 *   dma_offset— PCIe bus address offset
 */
int nvme_qpair_create(struct nvme_qpair *qp, uint16_t qid,
                      uint16_t depth, uint32_t dstrd,
                      uint64_t dma_offset);

/* Free DMA buffers and reset state. */
void nvme_qpair_destroy(struct nvme_qpair *qp);

/*
 * Submit a command to the SQ. Assigns a CID, copies the command
 * into the ring, flushes cache, and rings the doorbell.
 * Returns the assigned CID, or -1 if the queue is full.
 */
int nvme_qpair_submit(struct nvme_qpair *qp, struct nvme_cmd *cmd);

/*
 * Poll the CQ for completions. Processes ALL new completions
 * found (not just one). Updates the tracker table for each
 * completed CID. Returns the number of completions processed.
 */
int nvme_qpair_poll(struct nvme_qpair *qp);

/*
 * Submit a command and block until it completes (synchronous).
 * Combines submit + poll loop with timeout.
 *
 *   result_out — if non-NULL, receives CQE DW0
 *   Returns 0 on success, NVMe status field on error, -1 on timeout.
 */
int nvme_qpair_submit_sync(struct nvme_qpair *qp, struct nvme_cmd *cmd,
                           uint32_t *result_out, int timeout_ms);

/*
 * Check if a specific CID has completed.
 *   Returns 1 if complete (status written to *status_out), 0 if still pending.
 */
int nvme_qpair_cid_done(struct nvme_qpair *qp, uint16_t cid,
                        uint16_t *status_out, uint32_t *result_out);

/* Get the number of commands currently in flight. */
static inline uint16_t nvme_qpair_in_flight(const struct nvme_qpair *qp)
{
    return qp->cmds_in_flight;
}

/* Get the SQ bus address (for writing to ASQ or Create I/O SQ PRP1). */
static inline uint64_t nvme_qpair_sq_bus(const struct nvme_qpair *qp)
{
    return qp->sq.bus;
}

/* Get the CQ bus address. */
static inline uint64_t nvme_qpair_cq_bus(const struct nvme_qpair *qp)
{
    return qp->cq.bus;
}

#endif /* NVME_QUEUE_H */
