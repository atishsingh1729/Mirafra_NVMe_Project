#ifndef NVME_PCI_H
#define NVME_PCI_H
#include <stdint.h>
#include <stddef.h>

struct pci_device {
    char     bdf[16];
    char     sysfs_path[256];
    uint16_t vendor_id, device_id;
    uint32_t class_code;
    uint64_t bar0_phys, bar0_size;
    volatile void *bar0;
    uint64_t dma_offset;
    char     driver[64];
};

int  pci_find_nvme(struct pci_device *dev);
int  pci_unbind_driver(struct pci_device *dev);
int  pci_rebind_nvme_driver(struct pci_device *dev);
int  pci_enable_bus_master(struct pci_device *dev);
int  pci_map_bar0(struct pci_device *dev);
void pci_unmap_bar0(struct pci_device *dev);
void pci_detect_dma_offset(struct pci_device *dev);
int  pci_check_msix(struct pci_device *dev, uint16_t *table_size);

static inline uint64_t pci_phys_to_bus(const struct pci_device *dev, uint64_t phys) {
    return phys + dev->dma_offset;
}
#endif
