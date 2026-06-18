#ifndef NVME_IRQ_IOCTL_H
#define NVME_IRQ_IOCTL_H

/* Shared between kernel module and userspace */
#include <linux/ioctl.h>

#define NVME_IRQ_MAGIC 'N'
#define NVME_IRQ_SET_EVENTFD  _IOW(NVME_IRQ_MAGIC, 1, int)  /* pass eventfd to module */
#define NVME_IRQ_GET_INFO     _IOR(NVME_IRQ_MAGIC, 2, int)  /* get IRQ number */

#endif
