#ifndef NVME_ADMIN_H
#define NVME_ADMIN_H
#include "nvme_ctrl.h"

struct nvme_id_ctrl {
    uint16_t vid, ssvid;
    char sn[21], mn[41], fr[9];
    uint8_t mdts;
    uint16_t cntlid;
    uint32_t ver, nn;
    uint16_t oacs, oncs;
    uint8_t sqes, cqes, vwc;
};

struct nvme_id_ns {
    uint64_t nsze, ncap, nuse;
    uint8_t  flbas;
    uint32_t block_size;
    uint64_t capacity_bytes;
};

struct nvme_smart_log {
    uint8_t  critical_warning;
    uint16_t temperature_raw;
    int      temperature_celsius;
    uint8_t  available_spare, available_spare_threshold, percent_used;
    uint64_t data_units_read, data_units_written;
    uint64_t host_read_commands, host_write_commands;
    uint64_t controller_busy_time;
    uint64_t power_cycles, power_on_hours, unsafe_shutdowns;
    uint64_t media_errors, error_log_entries;
};

int  nvme_identify_controller(struct nvme_ctrl *ctrl, struct nvme_id_ctrl *id);
int  nvme_identify_namespace(struct nvme_ctrl *ctrl, uint32_t nsid, struct nvme_id_ns *ns);
int  nvme_get_smart_log(struct nvme_ctrl *ctrl, uint32_t nsid, struct nvme_smart_log *smart);
void nvme_print_id_ctrl(const struct nvme_id_ctrl *id);
void nvme_print_id_ns(const struct nvme_id_ns *ns, uint32_t nsid);
void nvme_print_smart(const struct nvme_smart_log *smart);

#endif
