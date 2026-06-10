/**
 * @file pci.h
 * @brief PCIe (Peripheral Component Interconnect Express) device
 *        discovery, BAR (Base Address Register) mapping, and
 *        driver bind/unbind helpers.
 *
 * Analogy: This module is the "phone book lookup" — it scans
 * the PCIe bus via sysfs, finds the NVMe device by class code,
 * reads its BAR addresses, and opens a portal (mmap) to its
 * register space.
 */

#ifndef NVME_PCI_H
#define NVME_PCI_H

#include <stdint.h>
#include <stddef.h>

/** Information about a discovered PCIe device */
struct pci_device {
    char     bdf[16];           /**< Bus:Device.Function string e.g. "0001:01:00.0" */
    char     sysfs_path[256];   /**< Full sysfs path                                */
    uint16_t vendor_id;         /**< PCI Vendor ID                                  */
    uint16_t device_id;         /**< PCI Device ID                                  */
    uint32_t class_code;        /**< PCI Class Code (24-bit)                        */

    /* BAR0 (Base Address Register 0) */
    uint64_t bar0_phys;         /**< BAR0 physical (CPU) address                    */
    uint64_t bar0_size;         /**< BAR0 size in bytes                             */
    volatile void *bar0;        /**< BAR0 mmap pointer (NULL until mapped)          */

    /* PCIe DMA (Direct Memory Access) inbound offset
     * On BCM2712 (RPi 5): bus_addr = cpu_phys + dma_offset
     * Auto-detected from device tree dma-ranges.                */
    uint64_t dma_offset;

    /* Driver state */
    char     driver[64];        /**< Bound driver name (empty if unbound)           */
};

/**
 * @brief Scan /sys/bus/pci/devices for an NVMe device.
 * @param dev  Output structure populated on success
 * @return 0 on success, -1 if no NVMe found
 *
 * Scans by PCI class code 0x010802 (Mass Storage / NVM / NVMe).
 * Fills vendor/device IDs, BAR0 address, and current driver.
 */
int pci_find_nvme(struct pci_device *dev);

/**
 * @brief Unbind the kernel driver from the device.
 * @param dev  Device to unbind
 * @return 0 on success, -1 on failure (or already unbound)
 */
int pci_unbind_driver(struct pci_device *dev);

/**
 * @brief Re-bind the kernel NVMe driver.
 * @param dev  Device to bind
 * @return 0 on success
 */
int pci_rebind_nvme_driver(struct pci_device *dev);

/**
 * @brief Ensure PCI bus mastering and memory space are enabled.
 * @param dev  Device to configure
 * @return 0 on success
 *
 * Bus mastering allows the NVMe controller to initiate DMA.
 * Memory space allows CPU access to BAR0 registers.
 */
int pci_enable_bus_master(struct pci_device *dev);

/**
 * @brief Map BAR0 into userspace via sysfs resource0.
 * @param dev  Device whose BAR0 to map
 * @return 0 on success (dev->bar0 set), -1 on failure
 */
int pci_map_bar0(struct pci_device *dev);

/**
 * @brief Unmap BAR0.
 * @param dev  Device whose BAR0 to unmap
 */
void pci_unmap_bar0(struct pci_device *dev);

/**
 * @brief Detect the PCIe inbound DMA offset from device tree.
 * @param dev  Device (dev->dma_offset set on return)
 *
 * On BCM2712 (Broadcom), the PCIe controller adds a fixed offset
 * when translating bus addresses to CPU physical addresses.
 * Default: 0x1000000000 if device tree lookup fails.
 */
void pci_detect_dma_offset(struct pci_device *dev);

/**
 * @brief Convert CPU physical address to PCIe bus address.
 * @param dev   Device context (for dma_offset)
 * @param phys  CPU physical address
 * @return PCIe bus address (phys + dma_offset)
 */
static inline uint64_t pci_phys_to_bus(const struct pci_device *dev, uint64_t phys)
{
    return phys + dev->dma_offset;
}

#endif /* NVME_PCI_H */
