/* milestone3.c — NVMe Admin Path: identify controller/namespace, SMART log */
#include "log.h"
#include "pci.h"
#include "mmio.h"
#include "nvme_ctrl.h"
#include "nvme_admin.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    struct pci_device pci; struct nvme_ctrl ctrl; int rc = 0;
    log_init((argc > 1 && argv[1][0] == '-' && argv[1][1] == 'q') ? LOG_INFO : LOG_TRACE, "nvme_m3.log");

    if (pci_find_nvme(&pci) < 0) { rc=1; goto done; }
    pci_detect_dma_offset(&pci);
    pci_unbind_driver(&pci); pci_enable_bus_master(&pci);
    if (pci_map_bar0(&pci) < 0) { rc=1; goto rebind; }
    mmio_init(pci.bar0);

    if (nvme_ctrl_init(&ctrl, &pci) < 0 || nvme_ctrl_reset_and_enable(&ctrl) < 0) { rc=1; goto cleanup; }

    struct nvme_id_ctrl id;
    if (nvme_identify_controller(&ctrl, &id) == 0) nvme_print_id_ctrl(&id);
    else { log_msg(LOG_ERROR, "Identify Controller failed"); rc=1; goto shutdown; }

    struct nvme_id_ns ns;
    if (nvme_identify_namespace(&ctrl, 1, &ns) == 0) nvme_print_id_ns(&ns, 1);

    struct nvme_smart_log smart;
    int sf = nvme_get_smart_log(&ctrl, 0xFFFFFFFF, &smart);
    if (sf != 0) sf = nvme_get_smart_log(&ctrl, 0, &smart);
    if (sf == 0) nvme_print_smart(&smart);
    else log_msg(LOG_ERROR, "SMART log failed SF=0x%04X", sf);

shutdown: nvme_ctrl_shutdown(&ctrl);
cleanup:  pci_unmap_bar0(&pci);
rebind:   pci_rebind_nvme_driver(&pci);
done:     log_shutdown(); return rc;
}
