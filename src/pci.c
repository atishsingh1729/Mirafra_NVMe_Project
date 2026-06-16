/**
 * @file pci.c
 * @brief PCIe device discovery, BAR mapping, and DMA offset detection.
 */

#include "pci.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>

#define PCI_SYSFS_DIR   "/sys/bus/pci/devices"
#define NVME_CLASS_CODE 0x0108  /* Mass Storage / NVM (top 16 bits of 24-bit class) */

/* ── Internal helpers ──────────────────────────────────────── */

static int read_sysfs_hex(const char *path, unsigned long *val)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = (fscanf(f, "%lx", val) == 1);
    fclose(f);
    return ok ? 0 : -1;
}

static int read_sysfs_str(const char *path, char *buf, size_t len)
{
    ssize_t n;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    /* Trim trailing newline */
    if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
    return 0;
}

/* ── Public API ────────────────────────────────────────────── */

int pci_find_nvme(struct pci_device *dev)
{
    DIR *dir = opendir(PCI_SYSFS_DIR);
    if (!dir) {
        log_msg(LOG_ERROR, "Cannot open %s", PCI_SYSFS_DIR);
        return -1;
    }

    memset(dev, 0, sizeof(*dev));
    struct dirent *entry;
    int found = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char path[512];
        unsigned long class_code = 0;
        snprintf(path, sizeof(path), "%s/%s/class", PCI_SYSFS_DIR, entry->d_name);
        if (read_sysfs_hex(path, &class_code) < 0) continue;
        if ((class_code >> 8) != NVME_CLASS_CODE) continue;

        /* Found an NVMe device */
        snprintf(dev->bdf, sizeof(dev->bdf), "%s", entry->d_name);
        snprintf(dev->sysfs_path, sizeof(dev->sysfs_path),
                 "%s/%s", PCI_SYSFS_DIR, entry->d_name);
        dev->class_code = (uint32_t)(class_code >> 8);

        /* Read vendor and device IDs */
        unsigned long val = 0;
        snprintf(path, sizeof(path), "%s/vendor", dev->sysfs_path);
        if (read_sysfs_hex(path, &val) == 0) dev->vendor_id = (uint16_t)val;

        snprintf(path, sizeof(path), "%s/device", dev->sysfs_path);
        if (read_sysfs_hex(path, &val) == 0) dev->device_id = (uint16_t)val;

        /* Read BAR0 from resource file (line 0: start end flags) */
        snprintf(path, sizeof(path), "%s/resource", dev->sysfs_path);
        FILE *rf = fopen(path, "r");
        if (rf) {
            unsigned long long start, end, flags;
            if (fscanf(rf, "%llx %llx %llx", &start, &end, &flags) == 3) {
                dev->bar0_phys = start;
                dev->bar0_size = end - start + 1;
            }
            fclose(rf);
        }

        /* Read bound driver name */
        char drv_link[512], drv_target[256];
        snprintf(drv_link, sizeof(drv_link), "%s/driver", dev->sysfs_path);
        ssize_t len = readlink(drv_link, drv_target, sizeof(drv_target) - 1);
        if (len > 0) {
            drv_target[len] = '\0';
            char *name = strrchr(drv_target, '/');
            snprintf(dev->driver, sizeof(dev->driver), "%s", name ? name + 1 : drv_target);
        }

        found = 1;
        break;
    }

    closedir(dir);

    if (!found) {
        log_msg(LOG_ERROR, "No NVMe device found on PCIe bus");
        return -1;
    }

    log_msg(LOG_DEBUG, "NVMe found: BDF=%s  VID=0x%04X  DID=0x%04X  BAR0=0x%llX (%llu KB)",
            dev->bdf, dev->vendor_id, dev->device_id,
            (unsigned long long)dev->bar0_phys,
            (unsigned long long)dev->bar0_size / 1024);

    if (dev->driver[0])
        log_msg(LOG_DEBUG, "Current driver: %s", dev->driver);
    else
        log_msg(LOG_DEBUG, "No driver bound");

    return 0;
}

int pci_unbind_driver(struct pci_device *dev)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/driver/unbind", dev->sysfs_path);

    FILE *f = fopen(path, "w");
    if (!f) {
        log_msg(LOG_DEBUG, "No driver to unbind (already unbound)");
        return 0;
    }
    fprintf(f, "%s", dev->bdf);
    fclose(f);
    usleep(500000); /* 500 ms for driver cleanup */

    dev->driver[0] = '\0';
    log_msg(LOG_DEBUG, "Kernel driver unbound from %s", dev->bdf);
    return 0;
}

int pci_rebind_nvme_driver(struct pci_device *dev)
{
    const char *bind_path = "/sys/bus/pci/drivers/nvme/bind";
    FILE *f = fopen(bind_path, "w");
    if (!f) {
        log_msg(LOG_WARN, "Cannot re-bind NVMe driver (fopen failed)");
        return -1;
    }
    fprintf(f, "%s", dev->bdf);
    fclose(f);
    snprintf(dev->driver, sizeof(dev->driver), "nvme");
    log_msg(LOG_DEBUG, "NVMe driver re-bound to %s", dev->bdf);
    return 0;
}

int pci_enable_bus_master(struct pci_device *dev)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/config", dev->sysfs_path);

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        log_msg(LOG_ERROR, "Cannot open PCI config space");
        return -1;
    }

    uint16_t cmd;
    pread(fd, &cmd, 2, 4); /* PCI Command register at offset 4 */
    uint16_t needed = 0x06; /* Bit 1 = Memory Space, Bit 2 = Bus Master */

    if ((cmd & needed) != needed) {
        cmd |= needed;
        pwrite(fd, &cmd, 2, 4);
        log_msg(LOG_DEBUG, "PCI Command register updated: bus mastering + mem space enabled");
    } else {
        log_msg(LOG_DEBUG, "Bus mastering already enabled (PCI CMD=0x%04X)", cmd);
    }

    close(fd);
    return 0;
}

int pci_map_bar0(struct pci_device *dev)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/resource0", dev->sysfs_path);

    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) {
        log_msg(LOG_ERROR, "Cannot open %s (run as root?)", path);
        return -1;
    }

    size_t map_size = (dev->bar0_size > 0) ? dev->bar0_size : 16384;
    dev->bar0 = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    close(fd);

    if (dev->bar0 == MAP_FAILED) {
        dev->bar0 = NULL;
        log_msg(LOG_ERROR, "mmap BAR0 failed");
        return -1;
    }

    log_msg(LOG_DEBUG, "BAR0 mapped: phys=0x%llX  virt=%p  size=%llu bytes",
            (unsigned long long)dev->bar0_phys, dev->bar0,
            (unsigned long long)map_size);
    return 0;
}

void pci_unmap_bar0(struct pci_device *dev)
{
    if (dev->bar0) {
        size_t map_size = (dev->bar0_size > 0) ? dev->bar0_size : 16384;
        munmap((void *)dev->bar0, map_size);
        dev->bar0 = NULL;
        log_msg(LOG_DEBUG, "BAR0 unmapped");
    }
}

void pci_detect_dma_offset(struct pci_device *dev)
{
    /* Try known device tree paths for RPi 5 PCIe dma-ranges */
    const char *dt_paths[] = {
        "/proc/device-tree/axi@1000000000/pcie@120000/dma-ranges",
        "/proc/device-tree/axi@1000000000/pcie@110000/dma-ranges",
        "/proc/device-tree/axi@1000000000/pcie@1100000/dma-ranges",
        NULL
    };

    for (int i = 0; dt_paths[i]; i++) {
        int fd = open(dt_paths[i], O_RDONLY);
        if (fd < 0) continue;

        uint32_t cells[8] = {0};
        ssize_t n = read(fd, cells, sizeof(cells));
        close(fd);

        if (n >= 20) {
            for (int j = 0; j < 8; j++)
                cells[j] = __builtin_bswap32(cells[j]);

            uint64_t bus_addr = ((uint64_t)cells[1] << 32) | cells[2];
            uint64_t cpu_addr = ((uint64_t)cells[3] << 32) | cells[4];
            dev->dma_offset = bus_addr - cpu_addr;

            log_msg(LOG_INFO, "DMA offset from device tree: 0x%llX (bus = phys + 0x%llX)",
                    (unsigned long long)dev->dma_offset,
                    (unsigned long long)dev->dma_offset);
            return;
        }
    }

    /* Default for BCM2712 (Broadcom SoC on RPi 5) */
    dev->dma_offset = 0x1000000000ULL;
    log_msg(LOG_DEBUG, "DMA offset: using BCM2712 default 0x%llX",
            (unsigned long long)dev->dma_offset);
}
