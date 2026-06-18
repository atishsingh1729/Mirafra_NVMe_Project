/* milestone1.c — Bring-up and Discovery: find device, map BAR0, dump registers */
#include "log.h"
#include "pci.h"
#include "mmio.h"
#include "nvme_regs.h"
#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    struct pci_device dev;
    log_init((argc > 1 && argv[1][0] == '-' && argv[1][1] == 'q') ? LOG_INFO : LOG_TRACE, "nvme_m1.log");

    if (pci_find_nvme(&dev) < 0) { log_msg(LOG_ERROR, "No NVMe found"); goto done; }
    pci_detect_dma_offset(&dev);
    if (pci_map_bar0(&dev) < 0) goto done;
    mmio_init(dev.bar0);

    uint64_t cap = mmio_read64(NVME_REG_CAP);
    uint32_t vs = mmio_read32(NVME_REG_VS);
    if (vs == 0xFFFFFFFF) { log_msg(LOG_ERROR, "PCIe link down"); goto cleanup; }

    log_msg(LOG_INFO, "NVMe %u.%u | MQES=%u | TO=%u ms | DSTRD=%u | MPS=%u-%u",
            VS_MAJOR(vs), VS_MINOR(vs), CAP_MQES(cap)+1, (unsigned)(CAP_TO(cap)*500),
            (unsigned)CAP_DSTRD(cap), 1<<(12+CAP_MPSMIN(cap)), 1<<(12+CAP_MPSMAX(cap)));

    uint32_t cc = mmio_read32(NVME_REG_CC);
    uint32_t csts = mmio_read32(NVME_REG_CSTS);
    log_msg(LOG_INFO, "CC=0x%08X [EN=%u] | CSTS=0x%08X [RDY=%u CFS=%u]",
            cc, cc & 1, csts, csts & 1, (csts >> 1) & 1);

    log_msg(LOG_INFO, "Register dump 0x00-0x3C:");
    for (uint32_t off = 0; off <= 0x3C; off += 4)
        log_msg(LOG_INFO, "  [0x%02X] = 0x%08X", off, mmio_read32(off));

cleanup:
    pci_unmap_bar0(&dev);
done:
    log_shutdown();
    return 0;
}
