#include "nvme_admin.h"
#include "dma.h"
#include "log.h"
#include <string.h>

static void trim(char *dst, const uint8_t *src, int len) {
    memcpy(dst, src, len); dst[len] = 0;
    for (int i = len - 1; i >= 0 && dst[i] == ' '; i--) dst[i] = 0;
}

static uint64_t le128_lo64(const uint8_t *p) {
    uint64_t v; memcpy(&v, p, 8); return v;
}

int nvme_identify_controller(struct nvme_ctrl *ctrl, struct nvme_id_ctrl *id) {
    struct dma_buffer buf;
    if (dma_alloc(&buf, 4096, ctrl->pci->dma_offset) < 0) return -1;

    struct nvme_cmd cmd = {0};
    cmd.opcode = NVME_ADMIN_IDENTIFY; cmd.prp1 = buf.bus; cmd.cdw10 = NVME_IDENTIFY_CTRL;
    int sf = nvme_admin_submit_sync(ctrl, &cmd, NULL, 5000);
    if (sf != 0) { dma_free(&buf); return sf < 0 ? -1 : sf; }

    dma_invalidate(buf.virt, 4096);
    uint8_t *d = buf.virt;
    memset(id, 0, sizeof(*id));
    memcpy(&id->vid, d, 2); memcpy(&id->ssvid, d+2, 2);
    trim(id->sn, d+4, 20); trim(id->mn, d+24, 40); trim(id->fr, d+64, 8);
    id->mdts = d[77]; memcpy(&id->cntlid, d+78, 2); memcpy(&id->ver, d+80, 4);
    memcpy(&id->oacs, d+256, 2);
    id->sqes = d[512]; id->cqes = d[513];
    memcpy(&id->nn, d+516, 4); memcpy(&id->oncs, d+520, 2); id->vwc = d[525];
    dma_free(&buf);
    return 0;
}

int nvme_identify_namespace(struct nvme_ctrl *ctrl, uint32_t nsid, struct nvme_id_ns *ns) {
    struct dma_buffer buf;
    if (dma_alloc(&buf, 4096, ctrl->pci->dma_offset) < 0) return -1;

    struct nvme_cmd cmd = {0};
    cmd.opcode = NVME_ADMIN_IDENTIFY; cmd.nsid = nsid; cmd.prp1 = buf.bus; cmd.cdw10 = NVME_IDENTIFY_NS;
    int sf = nvme_admin_submit_sync(ctrl, &cmd, NULL, 5000);
    if (sf != 0) { dma_free(&buf); return sf < 0 ? -1 : sf; }

    dma_invalidate(buf.virt, 4096);
    uint8_t *d = buf.virt;
    memset(ns, 0, sizeof(*ns));
    memcpy(&ns->nsze, d, 8); memcpy(&ns->ncap, d+8, 8); memcpy(&ns->nuse, d+16, 8);
    ns->flbas = d[26] & 0x0F;
    uint32_t fmt; memcpy(&fmt, d + 128 + ns->flbas * 4, 4);
    uint8_t lbads = (fmt >> 16) & 0xFF;
    ns->block_size = lbads ? (1U << lbads) : 0;
    ns->capacity_bytes = ns->nsze * ns->block_size;
    dma_free(&buf);
    return 0;
}

int nvme_get_smart_log(struct nvme_ctrl *ctrl, uint32_t nsid, struct nvme_smart_log *smart) {
    struct dma_buffer buf;
    if (dma_alloc(&buf, 512, ctrl->pci->dma_offset) < 0) return -1;

    struct nvme_cmd cmd = {0};
    cmd.opcode = NVME_ADMIN_GET_LOG_PAGE; cmd.nsid = nsid; cmd.prp1 = buf.bus;
    cmd.cdw10 = (127 << 16) | NVME_LOG_SMART;  /* 128 DWORDs = 512 bytes */

    int sf = nvme_admin_submit_sync(ctrl, &cmd, NULL, 5000);
    if (sf != 0) { dma_free(&buf); return sf < 0 ? -1 : sf; }

    dma_invalidate(buf.virt, 512);
    uint8_t *d = buf.virt;
    memset(smart, 0, sizeof(*smart));
    smart->critical_warning = d[0];
    memcpy(&smart->temperature_raw, d+1, 2);
    smart->temperature_celsius = (int)smart->temperature_raw - 273;
    smart->available_spare = d[3]; smart->available_spare_threshold = d[4]; smart->percent_used = d[5];
    smart->data_units_read = le128_lo64(d+32); smart->data_units_written = le128_lo64(d+48);
    smart->host_read_commands = le128_lo64(d+64); smart->host_write_commands = le128_lo64(d+80);
    smart->controller_busy_time = le128_lo64(d+96);
    smart->power_cycles = le128_lo64(d+112); smart->power_on_hours = le128_lo64(d+128);
    smart->unsafe_shutdowns = le128_lo64(d+144);
    smart->media_errors = le128_lo64(d+160); smart->error_log_entries = le128_lo64(d+176);
    dma_free(&buf);
    return 0;
}

void nvme_print_id_ctrl(const struct nvme_id_ctrl *id) {
    log_msg(LOG_INFO, "Controller: %s  SN=%s  FW=%s", id->mn, id->sn, id->fr);
    log_msg(LOG_INFO, "  NVMe %u.%u | VID=0x%04X | NN=%u | MDTS=%u | OACS=0x%04X | ONCS=0x%04X",
            (id->ver >> 16) & 0xFFFF, (id->ver >> 8) & 0xFF,
            id->vid, id->nn, id->mdts, id->oacs, id->oncs);
}

void nvme_print_id_ns(const struct nvme_id_ns *ns, uint32_t nsid) {
    log_msg(LOG_INFO, "NS%u: %llu LBAs x %u B = %llu GB",
            nsid, (unsigned long long)ns->nsze, ns->block_size,
            (unsigned long long)(ns->capacity_bytes / (1024ULL*1024*1024)));
}

void nvme_print_smart(const struct nvme_smart_log *s) {
    double gb_r = (double)s->data_units_read * 512000.0 / (1024.0*1024*1024);
    double gb_w = (double)s->data_units_written * 512000.0 / (1024.0*1024*1024);
    log_msg(LOG_INFO, "SMART: %d°C | spare=%u%% | used=%u%% | warn=0x%02X",
            s->temperature_celsius, s->available_spare, s->percent_used, s->critical_warning);
    log_msg(LOG_INFO, "  R=%.0f GB  W=%.0f GB  Power: %llu cycles, %llu hrs",
            gb_r, gb_w, (unsigned long long)s->power_cycles, (unsigned long long)s->power_on_hours);
    log_msg(LOG_INFO, "  Errors: %llu media, %llu log | Unsafe shutdowns: %llu",
            (unsigned long long)s->media_errors, (unsigned long long)s->error_log_entries,
            (unsigned long long)s->unsafe_shutdowns);
}
