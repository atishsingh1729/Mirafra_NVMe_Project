#ifndef NVME_CTRL_H
#define NVME_CTRL_H
#include "pci.h"
#include "dma.h"
#include "nvme_regs.h"

#define NVME_ADMIN_Q_DEPTH 32

struct nvme_ctrl {
    struct pci_device *pci;
    uint64_t cap;
    uint32_t doorbell_stride, timeout_ms;
    uint16_t max_queue_entries;
    struct dma_buffer admin_sq, admin_cq;
    uint16_t admin_sq_tail, admin_cq_head;
    uint8_t  admin_cq_phase;
    uint16_t cmd_id_counter;
};

int  nvme_ctrl_init(struct nvme_ctrl *ctrl, struct pci_device *pci);
int  nvme_ctrl_reset_and_enable(struct nvme_ctrl *ctrl);
void nvme_ctrl_shutdown(struct nvme_ctrl *ctrl);
int  nvme_admin_submit_sync(struct nvme_ctrl *ctrl, struct nvme_cmd *cmd,
                            uint32_t *result_out, int timeout_ms);
int  nvme_set_num_queues(struct nvme_ctrl *ctrl, uint16_t nsq, uint16_t ncq,
                         uint16_t *nsq_out, uint16_t *ncq_out);
#endif
