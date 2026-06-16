/**
 * @file nvme_admin.c
 * @brief NVMe admin command implementations with full data parsing.
 */

#include "nvme_admin.h"
#include "nvme_regs.h"
#include "dma.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

/* ── Helper: trim trailing spaces from a fixed-width string ─── */
static void trim_spaces(char *dst, const uint8_t *src, int len)
{
    memcpy(dst, src, len);
    dst[len] = '\0';
    for (int i = len - 1; i >= 0 && dst[i] == ' '; i--)
        dst[i] = '\0';
}

/* ── Helper: read lower 64 bits of a 128-bit LE field ───────── */
static uint64_t read_le128_low64(const uint8_t *p)
{
    uint64_t lo;
    memcpy(&lo, p, sizeof(lo));
    return lo;
}

/* =============================================================
 *  Identify Controller
 * ============================================================= */

int nvme_identify_controller(struct nvme_ctrl *ctrl, struct nvme_id_ctrl *id)
{
    struct dma_buffer buf;
    if (dma_alloc(&buf, 4096, ctrl->pci->dma_offset) < 0)
        return -1;

    struct nvme_cmd cmd = {0};
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid   = 0;
    cmd.prp1   = buf.bus;
    cmd.cdw10  = NVME_IDENTIFY_CTRL;   /* CNS (Controller or NS Structure) = 1 */

    int sf = nvme_admin_submit_sync(ctrl, &cmd, NULL, 5000);
    if (sf != 0) {
        dma_free(&buf);
        return (sf < 0) ? -1 : sf;
    }

    /* Invalidate cache before reading DMA'd data */
    dma_invalidate(buf.virt, 4096);
    uint8_t *d = (uint8_t *)buf.virt;

    memset(id, 0, sizeof(*id));

    /* Parse fields per NVMe spec §5.15.2.1 (Identify Controller) */
    memcpy(&id->vid,    d + 0,   2);
    memcpy(&id->ssvid,  d + 2,   2);
    trim_spaces(id->sn,  d + 4,  20);
    trim_spaces(id->mn,  d + 24, 40);
    trim_spaces(id->fr,  d + 64, 8);

    id->rab    = d[72];
    id->mdts   = d[77];
    memcpy(&id->cntlid, d + 78, 2);
    memcpy(&id->ver,    d + 80, 4);

    memcpy(&id->oacs,   d + 256, 2);
    id->acl  = d[258];
    id->aerl = d[259];
    id->frmw = d[260];
    id->lpa  = d[261];
    id->elpe = d[262];
    id->npss = d[263];

    id->sqes = d[512];
    id->cqes = d[513];
    memcpy(&id->nn,     d + 516, 4);
    memcpy(&id->oncs,   d + 520, 2);
    memcpy(&id->fuses,  d + 522, 2);
    id->vwc = d[525];

    dma_free(&buf);
    return 0;
}

/* =============================================================
 *  Identify Namespace
 * ============================================================= */

int nvme_identify_namespace(struct nvme_ctrl *ctrl, uint32_t nsid,
                            struct nvme_id_ns *ns)
{
    struct dma_buffer buf;
    if (dma_alloc(&buf, 4096, ctrl->pci->dma_offset) < 0)
        return -1;

    struct nvme_cmd cmd = {0};
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid   = nsid;
    cmd.prp1   = buf.bus;
    cmd.cdw10  = NVME_IDENTIFY_NS;   /* CNS = 0 */

    int sf = nvme_admin_submit_sync(ctrl, &cmd, NULL, 5000);
    if (sf != 0) {
        dma_free(&buf);
        return (sf < 0) ? -1 : sf;
    }

    dma_invalidate(buf.virt, 4096);
    uint8_t *d = (uint8_t *)buf.virt;

    memset(ns, 0, sizeof(*ns));

    /* Parse fields per NVMe spec §5.15.2.2 (Identify Namespace) */
    memcpy(&ns->nsze, d + 0,  8);
    memcpy(&ns->ncap, d + 8,  8);
    memcpy(&ns->nuse, d + 16, 8);
    ns->nsfeat = d[24];
    ns->nlbaf  = d[25];
    ns->flbas  = d[26];
    ns->mc     = d[27];
    ns->dpc    = d[28];
    ns->dps    = d[29];

    /* Parse all LBA (Logical Block Address) formats */
    ns->num_formats = (ns->nlbaf < 16) ? ns->nlbaf + 1 : 16;
    for (int i = 0; i < ns->num_formats; i++) {
        uint32_t raw;
        memcpy(&raw, d + 128 + i * 4, 4);
        ns->formats[i].ms    = (uint16_t)(raw & 0xFFFF);
        ns->formats[i].lbads = (uint8_t)((raw >> 16) & 0xFF);
        ns->formats[i].rp    = (uint8_t)((raw >> 24) & 0x3);
    }

    /* Compute active block size from the selected LBA format */
    uint8_t active_fmt = ns->flbas & 0x0F;
    if (active_fmt < ns->num_formats && ns->formats[active_fmt].lbads > 0) {
        ns->block_size = 1U << ns->formats[active_fmt].lbads;
        ns->capacity_bytes = ns->nsze * ns->block_size;
    }

    dma_free(&buf);
    return 0;
}

/* =============================================================
 *  SMART / Health Information Log
 * =============================================================
 *
 *  Get Log Page command (opcode 0x02):
 *    CDW10[7:0]   = LID  (Log Page Identifier) = 0x02 for SMART
 *    CDW10[27:16] = NUMDL (Number of DWORDs to return, lower 16 bits, 0-based)
 *    CDW11[15:0]  = NUMDU (upper 16 bits of NUMD)
 *    NSID         = 0xFFFFFFFF for global SMART
 *    PRP1         = buffer address (512 bytes for SMART)
 *
 *  The SMART log is exactly 512 bytes (128 DWORDs).
 *  NUMD (Number of Dwords, 0-based) = 128 - 1 = 127 = 0x7F
 */

int nvme_get_smart_log(struct nvme_ctrl *ctrl, uint32_t nsid,
                       struct nvme_smart_log *smart)
{
    struct dma_buffer buf;
    if (dma_alloc(&buf, 512, ctrl->pci->dma_offset) < 0)
        return -1;

    struct nvme_cmd cmd = {0};
    cmd.opcode = NVME_ADMIN_GET_LOG_PAGE;
    cmd.nsid   = nsid;
    cmd.prp1   = buf.bus;

    /* CDW10: LID=0x02 (SMART), NUMDL=127 (128 DWORDs = 512 bytes) */
    uint32_t numdl = 127;   /* 0-based: 128 DWORDs - 1 */
    cmd.cdw10  = (numdl << 16) | NVME_LOG_SMART;

    /* CDW11: NUMDU=0 (upper bits of NUMD, not needed for 512 bytes) */
    cmd.cdw11  = 0;

    int sf = nvme_admin_submit_sync(ctrl, &cmd, NULL, 5000);
    if (sf != 0) {
        dma_free(&buf);
        return (sf < 0) ? -1 : sf;
    }

    dma_invalidate(buf.virt, 512);
    uint8_t *d = (uint8_t *)buf.virt;

    memset(smart, 0, sizeof(*smart));

    /* Parse per NVMe spec §5.14.1.2 (SMART / Health Information Log) */

    /* Critical Warning (byte 0) — bitmap */
    smart->critical_warning          = d[0];
    smart->warn_spare_below_threshold = (d[0] >> 0) & 1;
    smart->warn_temperature           = (d[0] >> 1) & 1;
    smart->warn_reliability           = (d[0] >> 2) & 1;
    smart->warn_read_only             = (d[0] >> 3) & 1;
    smart->warn_volatile_backup       = (d[0] >> 4) & 1;

    /* Composite Temperature (bytes 1-2) — Kelvin */
    memcpy(&smart->temperature_raw, d + 1, 2);
    smart->temperature_celsius = (int)smart->temperature_raw - 273;

    /* Spare and wear (bytes 3-5) */
    smart->available_spare           = d[3];
    smart->available_spare_threshold = d[4];
    smart->percent_used              = d[5];

    /* I/O statistics (128-bit LE fields, we take lower 64 bits) */
    smart->data_units_read       = read_le128_low64(d + 32);
    smart->data_units_written    = read_le128_low64(d + 48);
    smart->host_read_commands    = read_le128_low64(d + 64);
    smart->host_write_commands   = read_le128_low64(d + 80);
    smart->controller_busy_time  = read_le128_low64(d + 96);

    /* Lifetime counters */
    smart->power_cycles          = read_le128_low64(d + 112);
    smart->power_on_hours        = read_le128_low64(d + 128);
    smart->unsafe_shutdowns      = read_le128_low64(d + 144);
    smart->media_errors          = read_le128_low64(d + 160);
    smart->error_log_entries     = read_le128_low64(d + 176);

    /* Temperature time (bytes 192-199) */
    memcpy(&smart->warning_temp_time,  d + 192, 4);
    memcpy(&smart->critical_temp_time, d + 196, 4);

    /* Temperature sensors (bytes 200-215, up to 8 × 2 bytes) */
    for (int i = 0; i < 8; i++)
        memcpy(&smart->temp_sensors[i], d + 200 + i * 2, 2);

    dma_free(&buf);
    return 0;
}

/* =============================================================
 *  Pretty-print functions
 * ============================================================= */

void nvme_print_id_ctrl(const struct nvme_id_ctrl *id)
{
    log_msg(LOG_INFO, "Controller: %s  SN=%s  FW=%s", id->mn, id->sn, id->fr);
    log_msg(LOG_INFO, "  NVMe %u.%u.%u | VID=0x%04X | CNTLID=%u | NN=%u | MDTS=%u",
            (id->ver >> 16) & 0xFFFF, (id->ver >> 8) & 0xFF, id->ver & 0xFF,
            id->vid, id->cntlid, id->nn, id->mdts);
    log_msg(LOG_INFO, "  OACS=0x%04X  ONCS=0x%04X  VWC=%s  SQES=%u/%u  CQES=%u/%u",
            id->oacs, id->oncs, (id->vwc & 1) ? "yes" : "no",
            1 << (id->sqes & 0xF), 1 << ((id->sqes >> 4) & 0xF),
            1 << (id->cqes & 0xF), 1 << ((id->cqes >> 4) & 0xF));
}

void nvme_print_id_ns(const struct nvme_id_ns *ns, uint32_t nsid)
{
    log_msg(LOG_INFO, "Namespace %u: %llu LBAs x %u B = %llu GB  (used: %llu LBAs)",
            nsid, (unsigned long long)ns->nsze, ns->block_size,
            (unsigned long long)(ns->capacity_bytes / (1024ULL * 1024 * 1024)),
            (unsigned long long)ns->nuse);
}

void nvme_print_smart(const struct nvme_smart_log *smart)
{
    double gb_r = (double)smart->data_units_read * 512000.0 / (1024.0 * 1024 * 1024);
    double gb_w = (double)smart->data_units_written * 512000.0 / (1024.0 * 1024 * 1024);

    log_msg(LOG_INFO, "SMART: %d°C | spare=%u%% | used=%u%% | warn=0x%02X",
            smart->temperature_celsius, smart->available_spare,
            smart->percent_used, smart->critical_warning);
    log_msg(LOG_INFO, "  Read=%.0f GB  Written=%.0f GB  Busy=%llu min",
            gb_r, gb_w, (unsigned long long)smart->controller_busy_time);
    log_msg(LOG_INFO, "  Power: %llu cycles, %llu hrs, %llu unsafe shutdowns",
            (unsigned long long)smart->power_cycles,
            (unsigned long long)smart->power_on_hours,
            (unsigned long long)smart->unsafe_shutdowns);
    log_msg(LOG_INFO, "  Errors: %llu media, %llu log entries",
            (unsigned long long)smart->media_errors,
            (unsigned long long)smart->error_log_entries);
}
