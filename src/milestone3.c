/*
 * milestone3.c — NVMe Admin Path
 *
 * Reset controller, identify controller and namespace,
 * pull SMART health log. Print results and exit.
 */

#include "log.h"
#include "pci.h"
#include "mmio.h"
#include "nvme_ctrl.h"
#include "nvme_admin.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    struct pci_device pci_dev;
    struct nvme_ctrl ctrl;
    int rc = 0;

    log_init((argc > 1 && argv[1][0] == '-' && argv[1][1] == 'q')
             ? LOG_INFO : LOG_TRACE, "nvme_m3.log");

    /* Find and take over the NVMe device */
    if (pci_find_nvme(&pci_dev) < 0) { rc = 1; goto done; }
    pci_detect_dma_offset(&pci_dev);
    pci_unbind_driver(&pci_dev);
    pci_enable_bus_master(&pci_dev);
    if (pci_map_bar0(&pci_dev) < 0) { rc = 1; goto rebind; }
    mmio_init(pci_dev.bar0);

    /* Reset and enable controller (sets up admin queues internally) */
    if (nvme_ctrl_init(&ctrl, &pci_dev) < 0) { rc = 1; goto cleanup; }
    if (nvme_ctrl_reset_and_enable(&ctrl) < 0) { rc = 1; goto cleanup; }

    /* Identify Controller */
    struct nvme_id_ctrl id_ctrl;
    int sf = nvme_identify_controller(&ctrl, &id_ctrl);
    if (sf != 0) {
        log_msg(LOG_ERROR, "Identify Controller failed (SF=0x%04X)", sf);
        rc = 1; goto shutdown;
    }
    nvme_print_id_ctrl(&id_ctrl);

    /* Identify all namespaces */
    uint32_t num_ns = id_ctrl.nn ? id_ctrl.nn : 1;
    if (num_ns > 8) num_ns = 8;

    for (uint32_t nsid = 1; nsid <= num_ns; nsid++) {
        struct nvme_id_ns id_ns;
        sf = nvme_identify_namespace(&ctrl, nsid, &id_ns);
        if (sf != 0 || id_ns.nsze == 0) continue;
        nvme_print_id_ns(&id_ns, nsid);
    }

    /* SMART / Health log */
    struct nvme_smart_log smart;
    sf = nvme_get_smart_log(&ctrl, 0xFFFFFFFF, &smart);
    if (sf != 0) sf = nvme_get_smart_log(&ctrl, 0, &smart);

    if (sf == 0)
        nvme_print_smart(&smart);
    else
        log_msg(LOG_ERROR, "SMART log failed (SF=0x%04X)", sf);

shutdown:
    nvme_ctrl_shutdown(&ctrl);
cleanup:
    pci_unmap_bar0(&pci_dev);
rebind:
    pci_rebind_nvme_driver(&pci_dev);
done:
    log_shutdown();
    return rc;
}
