#include "pci.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>

static int read_sysfs_hex(const char *path, unsigned long *val) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = (fscanf(f, "%lx", val) == 1);
    fclose(f);
    return ok ? 0 : -1;
}

int pci_find_nvme(struct pci_device *dev) {
    DIR *dir = opendir("/sys/bus/pci/devices");
    if (!dir) return -1;
    memset(dev, 0, sizeof(*dev));
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[512]; unsigned long cls = 0;
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/class", e->d_name);
        if (read_sysfs_hex(path, &cls) < 0 || (cls >> 8) != 0x0108) continue;

        snprintf(dev->bdf, sizeof(dev->bdf), "%s", e->d_name);
        snprintf(dev->sysfs_path, sizeof(dev->sysfs_path), "/sys/bus/pci/devices/%s", e->d_name);
        dev->class_code = (uint32_t)(cls >> 8);

        unsigned long v = 0;
        snprintf(path, sizeof(path), "%s/vendor", dev->sysfs_path);
        if (read_sysfs_hex(path, &v) == 0) dev->vendor_id = (uint16_t)v;
        snprintf(path, sizeof(path), "%s/device", dev->sysfs_path);
        if (read_sysfs_hex(path, &v) == 0) dev->device_id = (uint16_t)v;

        snprintf(path, sizeof(path), "%s/resource", dev->sysfs_path);
        FILE *rf = fopen(path, "r");
        if (rf) {
            unsigned long long s, e2, f;
            if (fscanf(rf, "%llx %llx %llx", &s, &e2, &f) == 3) {
                dev->bar0_phys = s; dev->bar0_size = e2 - s + 1;
            }
            fclose(rf);
        }

        char dl[512], dt[256];
        snprintf(dl, sizeof(dl), "%s/driver", dev->sysfs_path);
        ssize_t len = readlink(dl, dt, sizeof(dt) - 1);
        if (len > 0) { dt[len] = 0; char *n = strrchr(dt, '/'); snprintf(dev->driver, sizeof(dev->driver), "%s", n ? n+1 : dt); }

        closedir(dir);
        log_msg(LOG_DEBUG, "NVMe: %s VID=0x%04X DID=0x%04X BAR0=0x%llX (%llu KB)",
                dev->bdf, dev->vendor_id, dev->device_id,
                (unsigned long long)dev->bar0_phys, (unsigned long long)dev->bar0_size/1024);
        return 0;
    }
    closedir(dir);
    return -1;
}

int pci_unbind_driver(struct pci_device *dev) {
    char path[512];
    snprintf(path, sizeof(path), "%s/driver/unbind", dev->sysfs_path);
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "%s", dev->bdf); fclose(f);
    usleep(500000);
    dev->driver[0] = 0;
    return 0;
}

int pci_rebind_nvme_driver(struct pci_device *dev) {
    FILE *f = fopen("/sys/bus/pci/drivers/nvme/bind", "w");
    if (!f) return -1;
    fprintf(f, "%s", dev->bdf); fclose(f);
    return 0;
}

int pci_enable_bus_master(struct pci_device *dev) {
    char path[512];
    snprintf(path, sizeof(path), "%s/config", dev->sysfs_path);
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    uint16_t cmd;
    pread(fd, &cmd, 2, 4);
    if (!(cmd & 0x06)) { cmd |= 0x06; pwrite(fd, &cmd, 2, 4); }
    close(fd);
    return 0;
}

int pci_map_bar0(struct pci_device *dev) {
    char path[512];
    snprintf(path, sizeof(path), "%s/resource0", dev->sysfs_path);
    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) return -1;
    size_t sz = dev->bar0_size > 0 ? dev->bar0_size : 16384;
    dev->bar0 = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (dev->bar0 == MAP_FAILED) { dev->bar0 = NULL; return -1; }
    return 0;
}

void pci_unmap_bar0(struct pci_device *dev) {
    if (dev->bar0) {
        munmap((void *)dev->bar0, dev->bar0_size > 0 ? dev->bar0_size : 16384);
        dev->bar0 = NULL;
    }
}

void pci_detect_dma_offset(struct pci_device *dev) {
    const char *paths[] = {
        "/proc/device-tree/axi@1000000000/pcie@120000/dma-ranges",
        "/proc/device-tree/axi@1000000000/pcie@110000/dma-ranges",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        int fd = open(paths[i], O_RDONLY);
        if (fd < 0) continue;
        uint32_t cells[8] = {0};
        ssize_t n = read(fd, cells, sizeof(cells)); close(fd);
        if (n >= 20) {
            for (int j = 0; j < 8; j++) cells[j] = __builtin_bswap32(cells[j]);
            uint64_t bus = ((uint64_t)cells[1] << 32) | cells[2];
            uint64_t cpu = ((uint64_t)cells[3] << 32) | cells[4];
            dev->dma_offset = bus - cpu;
            return;
        }
    }
    dev->dma_offset = 0x1000000000ULL;
}

/* Walk PCI capability list to find MSI-X and read table size */
int pci_check_msix(struct pci_device *dev, uint16_t *table_size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/config", dev->sysfs_path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    /* Read capabilities pointer at config offset 0x34 */
    uint8_t cap_ptr;
    if (pread(fd, &cap_ptr, 1, 0x34) != 1) { close(fd); return -1; }

    /* Walk the capability linked list */
    while (cap_ptr && cap_ptr != 0xFF) {
        uint8_t cap_id, cap_next;
        pread(fd, &cap_id, 1, cap_ptr);
        pread(fd, &cap_next, 1, cap_ptr + 1);

        if (cap_id == 0x11) {  /* MSI-X capability */
            uint16_t msg_ctrl;
            pread(fd, &msg_ctrl, 2, cap_ptr + 2);
            *table_size = (msg_ctrl & 0x7FF) + 1;  /* bits 10:0 = table size - 1 */
            close(fd);
            return 0;
        }
        cap_ptr = cap_next;
    }
    close(fd);
    return -1;  /* MSI-X not found */
}
