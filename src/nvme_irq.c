/*
 * nvme_irq.c — Interrupt delivery: kernel module → UIO → polling.
 *
 * Kernel module path:
 *   1. Open /dev/nvme_irq (created by nvme_irq.ko)
 *   2. Create an eventfd
 *   3. Pass eventfd to module via ioctl(NVME_IRQ_SET_EVENTFD)
 *   4. read() on eventfd blocks until MSI-X fires
 */
#include "nvme_irq.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <linux/ioctl.h>

/* ioctl commands — must match kernel module */
#define NVME_IRQ_MAGIC 'N'
#define NVME_IRQ_SET_EVENTFD  _IOW(NVME_IRQ_MAGIC, 1, int)

/* Try kernel module path: /dev/nvme_irq + eventfd */
static int try_kmod(struct nvme_irq *irq) {
    irq->kmod_fd = open("/dev/nvme_irq", O_RDWR);
    if (irq->kmod_fd < 0) return -1;

    irq->event_fd = eventfd(0, EFD_NONBLOCK);
    if (irq->event_fd < 0) { close(irq->kmod_fd); irq->kmod_fd = -1; return -1; }

    if (ioctl(irq->kmod_fd, NVME_IRQ_SET_EVENTFD, irq->event_fd) < 0) {
        close(irq->event_fd); close(irq->kmod_fd);
        irq->event_fd = -1; irq->kmod_fd = -1;
        return -1;
    }

    irq->mode = NVME_IRQ_MODE_KMOD;
    return 0;
}

/* Try UIO path */
static int try_uio(struct nvme_irq *irq, struct pci_device *dev) {
    if (system("modprobe uio_pci_generic 2>/dev/null") != 0) return -1;

    /* Bind to uio_pci_generic */
    char path[256];
    FILE *f;
    snprintf(path, sizeof(path), "/sys/bus/pci/drivers/uio_pci_generic/new_id");
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%04x %04x", dev->vendor_id, dev->device_id); fclose(f);

    snprintf(path, sizeof(path), "%s/driver_override", dev->sysfs_path);
    f = fopen(path, "w");
    if (f) { fprintf(f, "uio_pci_generic"); fclose(f); }

    f = fopen("/sys/bus/pci/drivers_probe", "w");
    if (f) { fprintf(f, "%s", dev->bdf); fclose(f); }
    usleep(200000);

    /* Find /dev/uioN */
    char uio_dir[256];
    snprintf(uio_dir, sizeof(uio_dir), "%s/uio", dev->sysfs_path);
    int devnum = -1;
    DIR *dir = opendir(uio_dir);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir)) != NULL)
            if (sscanf(e->d_name, "uio%d", &devnum) == 1) break;
        closedir(dir);
    }
    if (devnum < 0) return -1;

    char devpath[32];
    snprintf(devpath, sizeof(devpath), "/dev/uio%d", devnum);
    irq->uio_fd = open(devpath, O_RDWR);
    if (irq->uio_fd < 0) return -1;

    uint32_t enable = 1;
    write(irq->uio_fd, &enable, sizeof(enable));
    irq->mode = NVME_IRQ_MODE_UIO;
    return 0;
}

int nvme_irq_init(struct nvme_irq *irq, struct pci_device *dev) {
    memset(irq, 0, sizeof(*irq));
    irq->kmod_fd = -1; irq->event_fd = -1; irq->uio_fd = -1;
    irq->mode = NVME_IRQ_MODE_POLL;

    pci_check_msix(dev, &irq->msix_vectors);

    /* Priority 1: kernel module */
    if (try_kmod(irq) == 0) {
        log_msg(LOG_INFO, "IRQ: using kernel module (/dev/nvme_irq + eventfd), MSI-X");
        return 0;
    }

    /* Priority 2: UIO */
    if (try_uio(irq, dev) == 0) {
        log_msg(LOG_INFO, "IRQ: using uio_pci_generic (/dev/uio)");
        return 0;
    }

    /* Priority 3: polling */
    log_msg(LOG_INFO, "IRQ: using polling fallback (no kmod, no UIO)");
    return 0;
}

int nvme_irq_wait(struct nvme_irq *irq, int timeout_ms) {
    if (irq->mode == NVME_IRQ_MODE_KMOD && irq->event_fd >= 0) {
        /* Block on eventfd — wakes when kernel module ISR signals it */
        fd_set fds; FD_ZERO(&fds); FD_SET(irq->event_fd, &fds);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        int ret = select(irq->event_fd + 1, &fds, NULL, NULL, &tv);
        if (ret > 0) {
            uint64_t count;
            read(irq->event_fd, &count, sizeof(count));
            return 1;
        }
        return 0;
    }

    if (irq->mode == NVME_IRQ_MODE_UIO && irq->uio_fd >= 0) {
        fd_set fds; FD_ZERO(&fds); FD_SET(irq->uio_fd, &fds);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        int ret = select(irq->uio_fd + 1, &fds, NULL, NULL, &tv);
        if (ret > 0) {
            uint32_t count;
            read(irq->uio_fd, &count, sizeof(count));
            return 1;
        }
        return 0;
    }

    return 0;  /* poll mode */
}

void nvme_irq_ack(struct nvme_irq *irq) {
    if (irq->mode == NVME_IRQ_MODE_UIO && irq->uio_fd >= 0) {
        uint32_t enable = 1;
        write(irq->uio_fd, &enable, sizeof(enable));
    }
    /* kmod: eventfd auto-clears on read, no ack needed */
}

void nvme_irq_shutdown(struct nvme_irq *irq) {
    if (irq->event_fd >= 0) close(irq->event_fd);
    if (irq->kmod_fd >= 0) close(irq->kmod_fd);
    if (irq->uio_fd >= 0) close(irq->uio_fd);
    irq->event_fd = -1; irq->kmod_fd = -1; irq->uio_fd = -1;
    irq->mode = NVME_IRQ_MODE_POLL;
}

const char *nvme_irq_mode_str(const struct nvme_irq *irq) {
    switch (irq->mode) {
    case NVME_IRQ_MODE_KMOD: return "MSI-X (kernel module + eventfd)";
    case NVME_IRQ_MODE_UIO:  return "UIO (uio_pci_generic)";
    case NVME_IRQ_MODE_POLL: return "Polling (CQ phase bit)";
    default:                 return "Unknown";
    }
}
