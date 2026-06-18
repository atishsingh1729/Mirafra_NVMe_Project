/* milestone2.c — MMIO and DMA: BAR validation, buffer allocation, DMA round-trip */
#include "log.h"
#include "pci.h"
#include "mmio.h"
#include "dma.h"
#include "nvme_regs.h"
#include "nvme_ctrl.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    struct pci_device pci; struct nvme_ctrl ctrl;
    struct dma_buffer bufs[4]; int rc = 0;
    memset(bufs, 0, sizeof(bufs));
    log_init((argc > 1 && argv[1][0] == '-' && argv[1][1] == 'q') ? LOG_INFO : LOG_TRACE, "nvme_m2.log");

    if (pci_find_nvme(&pci) < 0) { rc=1; goto done; }
    pci_detect_dma_offset(&pci);
    log_msg(LOG_INFO, "BAR0: 0x%llX  size=%llu  aligned=%s",
            (unsigned long long)pci.bar0_phys, (unsigned long long)pci.bar0_size,
            (pci.bar0_phys % 4096 == 0) ? "yes" : "no");

    pci_unbind_driver(&pci); pci_enable_bus_master(&pci);
    if (pci_map_bar0(&pci) < 0) { rc=1; goto rebind; }
    mmio_init(pci.bar0);

    for (int i = 0; i < 4; i++) {
        if (dma_alloc(&bufs[i], 4096, pci.dma_offset) < 0) { rc=1; goto cleanup; }
        log_msg(LOG_INFO, "  buf[%d]: phys=0x%llX bus=0x%llX aligned=%s", i,
                (unsigned long long)bufs[i].phys, (unsigned long long)bufs[i].bus,
                (bufs[i].phys % 4096 == 0) ? "yes" : "no");
    }

    if (nvme_ctrl_init(&ctrl, &pci) < 0 || nvme_ctrl_reset_and_enable(&ctrl) < 0) { rc=1; goto cleanup; }

    struct nvme_cmd cmd = {0};
    cmd.opcode = NVME_ADMIN_IDENTIFY; cmd.prp1 = bufs[0].bus; cmd.cdw10 = NVME_IDENTIFY_CTRL;
    int sf = nvme_admin_submit_sync(&ctrl, &cmd, NULL, 5000);
    if (sf == 0) {
        dma_invalidate(bufs[0].virt, 4096);
        char sn[21]={0}, mn[41]={0}; memcpy(sn, (uint8_t*)bufs[0].virt+4, 20); memcpy(mn, (uint8_t*)bufs[0].virt+24, 40);
        log_msg(LOG_INFO, "DMA round-trip OK: %s SN=%s", mn, sn);
    } else { log_msg(LOG_ERROR, "DMA round-trip FAILED SF=0x%04X", sf); rc=1; }

    nvme_ctrl_shutdown(&ctrl);
cleanup:
    for (int i = 0; i < 4; i++) dma_free(&bufs[i]);
    pci_unmap_bar0(&pci);
rebind:
    pci_rebind_nvme_driver(&pci);
done:
    log_shutdown(); return rc;
}
