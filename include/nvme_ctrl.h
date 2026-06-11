/**
 * @file nvme_ctrl.h
 * @brief NVMe controller lifecycle: reset, admin queue setup,
 *        enable, command submission, and completion polling.
 *
 * This module owns the controller state machine and the admin
 * queue pair. I/O queues will be managed by nvme_queue.h (M4).
 *
 * Analogy: This is the "restaurant manager" — opens/closes the
 * restaurant (enable/disable), installs the order pad and receipt
 * counter (admin queues), and handles management requests (admin
 * commands like Identify and Set Features).
 */

#ifndef NVME_CTRL_H
#define NVME_CTRL_H

#include "pci.h"
#include "dma.h"
#include "nvme_regs.h"
#include <stdint.h>

#define NVME_ADMIN_Q_DEPTH  32  /**< Admin queue depth (entries)        */

/** Controller state — tracks everything needed for admin ops */
struct nvme_ctrl {
    struct pci_device *pci;         /**< PCIe device context            */

    /* Cached CAP (Controller Capabilities) fields */
    uint64_t cap;                   /**< Raw CAP register               */
    uint32_t doorbell_stride;       /**< 4 << CAP.DSTRD                 */
    uint32_t timeout_ms;            /**< CAP.TO × 500 ms                */
    uint16_t max_queue_entries;     /**< CAP.MQES + 1                   */

    /* Admin queue pair */
    struct dma_buffer admin_sq;     /**< Admin Submission Queue buffer  */
    struct dma_buffer admin_cq;     /**< Admin Completion Queue buffer  */
    uint16_t admin_sq_tail;         /**< SQ tail index (next write pos) */
    uint16_t admin_cq_head;         /**< CQ head index (next read pos)  */
    uint8_t  admin_cq_phase;        /**< Expected CQ phase bit          */
    uint16_t cmd_id_counter;        /**< Global CID (Command ID) counter */
};

/**
 * @brief Initialise controller context and cache CAP fields.
 * @param ctrl  Controller state to initialise
 * @param pci   Populated PCI device (BAR0 must be mapped)
 * @return 0 on success
 *
 * Does NOT reset or enable the controller — just reads CAP.
 */
int nvme_ctrl_init(struct nvme_ctrl *ctrl, struct pci_device *pci);

/**
 * @brief Full controller reset and bring-up sequence.
 * @param ctrl  Controller state
 * @return 0 on success
 *
 * Sequence:
 *   1. Disable controller (CC.EN=0, wait CSTS.RDY=0)
 *   2. Allocate admin SQ/CQ DMA buffers
 *   3. Write AQA, ASQ, ACQ registers
 *   4. Enable controller (CC.EN=1, wait CSTS.RDY=1)
 */
int nvme_ctrl_reset_and_enable(struct nvme_ctrl *ctrl);

/**
 * @brief Disable the controller and free admin queue buffers.
 * @param ctrl  Controller state
 */
void nvme_ctrl_shutdown(struct nvme_ctrl *ctrl);

/**
 * @brief Submit an admin command and poll for completion.
 * @param ctrl        Controller state
 * @param cmd         64-byte command to submit (CID set automatically)
 * @param result_out  If non-NULL, receives CQE DW0 (command-specific result)
 * @param timeout_ms  Poll timeout in milliseconds
 * @return 0 on success, or NVMe status field (non-zero = error)
 *
 * This is a synchronous (blocking) call: submit, poll, return.
 */
int nvme_admin_submit_sync(struct nvme_ctrl *ctrl,
                           struct nvme_cmd *cmd,
                           uint32_t *result_out,
                           int timeout_ms);

/**
 * @brief Send Set Features: Number of Queues.
 * @param ctrl    Controller state
 * @param nsq     Number of I/O SQs requested (1-based)
 * @param ncq     Number of I/O CQs requested (1-based)
 * @param nsq_out Actual SQs allocated (1-based, output)
 * @param ncq_out Actual CQs allocated (1-based, output)
 * @return 0 on success
 */
int nvme_set_num_queues(struct nvme_ctrl *ctrl,
                        uint16_t nsq, uint16_t ncq,
                        uint16_t *nsq_out, uint16_t *ncq_out);

#endif /* NVME_CTRL_H */
