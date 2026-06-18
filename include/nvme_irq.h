/*
 * nvme_irq.h — Interrupt delivery abstraction.
 *
 * Priority order:
 *   1. Kernel module (/dev/nvme_irq) — MSI-X via eventfd
 *   2. uio_pci_generic — legacy interrupt via /dev/uioN
 *   3. Polling — CQ phase bit spin
 */
#ifndef NVME_IRQ_H
#define NVME_IRQ_H

#include "pci.h"
#include <stdint.h>

#define NVME_IRQ_MODE_POLL  0
#define NVME_IRQ_MODE_UIO   1
#define NVME_IRQ_MODE_KMOD  2  /* kernel module + eventfd */

struct nvme_irq {
    int mode;
    int kmod_fd;        /* fd for /dev/nvme_irq        */
    int event_fd;       /* eventfd for interrupt signal */
    int uio_fd;         /* fd for /dev/uioN            */
    uint16_t msix_vectors;
};

int  nvme_irq_init(struct nvme_irq *irq, struct pci_device *dev);
int  nvme_irq_wait(struct nvme_irq *irq, int timeout_ms);
void nvme_irq_ack(struct nvme_irq *irq);
void nvme_irq_shutdown(struct nvme_irq *irq);
const char *nvme_irq_mode_str(const struct nvme_irq *irq);

#endif
