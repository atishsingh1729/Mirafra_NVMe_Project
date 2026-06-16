/**
 * @file nvme_admin.h
 * @brief NVMe admin command wrappers: Identify Controller,
 *        Identify Namespace, and SMART/Health log.
 *
 * Each function allocates its own DMA buffer, submits the admin
 * command, and parses the returned data into a C struct.
 *
 * Analogy: These are the "front desk inquiries" at the restaurant.
 *   Identify Controller = "What's your name and capacity?"
 *   Identify Namespace  = "How big is this specific storage room?"
 *   SMART/Health log    = "Show me your health inspection report."
 */

#ifndef NVME_ADMIN_H
#define NVME_ADMIN_H

#include "nvme_ctrl.h"
#include <stdint.h>

/* =============================================================
 *  Identify Controller — parsed fields (NVMe spec §5.15.2.1)
 * ============================================================= */

struct nvme_id_ctrl {
    /* Identity */
    uint16_t vid;               /**< PCI Vendor ID                          */
    uint16_t ssvid;             /**< PCI Subsystem Vendor ID                */
    char     sn[21];            /**< Serial Number (null-terminated)        */
    char     mn[41];            /**< Model Number (null-terminated)         */
    char     fr[9];             /**< Firmware Revision (null-terminated)    */

    /* Capabilities */
    uint8_t  rab;               /**< Recommended Arbitration Burst          */
    uint8_t  mdts;              /**< Max Data Transfer Size (power of 2, in MPS units) */
    uint16_t cntlid;            /**< Controller ID                          */
    uint32_t ver;               /**< Version (same encoding as VS register) */

    /* Admin capabilities */
    uint16_t oacs;              /**< Optional Admin Command Support bitmap  */
    uint8_t  acl;               /**< Abort Command Limit                    */
    uint8_t  aerl;              /**< Async Event Request Limit              */
    uint8_t  frmw;              /**< Firmware Updates capabilities          */
    uint8_t  lpa;               /**< Log Page Attributes                    */
    uint8_t  elpe;              /**< Error Log Page Entries (0-based)       */
    uint8_t  npss;              /**< Number of Power States Support (0-based) */

    /* NVM capabilities */
    uint8_t  sqes;              /**< SQ Entry Size (min/max, nibbles)       */
    uint8_t  cqes;              /**< CQ Entry Size (min/max, nibbles)       */
    uint32_t nn;                /**< Number of Namespaces                   */
    uint16_t oncs;              /**< Optional NVM Command Support bitmap    */
    uint16_t fuses;             /**< Fused Operation Support                */
    uint8_t  vwc;               /**< Volatile Write Cache                   */
};

/* =============================================================
 *  Identify Namespace — parsed fields (NVMe spec §5.15.2.2)
 * ============================================================= */

struct nvme_lba_format {
    uint16_t ms;                /**< Metadata Size (bytes)                  */
    uint8_t  lbads;             /**< LBA Data Size (power of 2)             */
    uint8_t  rp;                /**< Relative Performance (0=best, 3=worst) */
};

struct nvme_id_ns {
    uint64_t nsze;              /**< Namespace Size (total LBAs)            */
    uint64_t ncap;              /**< Namespace Capacity (usable LBAs)       */
    uint64_t nuse;              /**< Namespace Utilization (used LBAs)      */
    uint8_t  nsfeat;            /**< Namespace Features bitmap              */
    uint8_t  nlbaf;             /**< Number of LBA Formats (0-based)        */
    uint8_t  flbas;             /**< Formatted LBA Size index + metadata    */
    uint8_t  mc;                /**< Metadata Capabilities                  */
    uint8_t  dpc;               /**< Data Protection Capabilities           */
    uint8_t  dps;               /**< Data Protection Settings               */

    /* Active LBA format */
    uint32_t block_size;        /**< Computed: 2^lbads (bytes per block)    */
    uint64_t capacity_bytes;    /**< Computed: nsze × block_size            */

    /* All LBA formats (up to 16) */
    uint8_t              num_formats;
    struct nvme_lba_format formats[16];
};

/* =============================================================
 *  SMART / Health Information — parsed (NVMe spec §5.14.1.2)
 * =============================================================
 *
 *  The SMART log is the drive's health inspection report.
 *  Critical warnings tell you if the drive is about to fail.
 *  Temperature, wear, and error counts tell you how hard
 *  the drive has been working.
 *
 *  Note: Many fields are 128-bit counters. We store the lower
 *  64 bits, which is sufficient for practical values.          */

struct nvme_smart_log {
    /* Critical Warning bitmap (byte 0) */
    uint8_t  critical_warning;
    int      warn_spare_below_threshold;  /**< Bit 0: available spare low  */
    int      warn_temperature;            /**< Bit 1: temperature exceeded */
    int      warn_reliability;            /**< Bit 2: reliability degraded */
    int      warn_read_only;              /**< Bit 3: media placed in RO   */
    int      warn_volatile_backup;        /**< Bit 4: volatile mem backup  */

    /* Temperature */
    uint16_t temperature_raw;             /**< Kelvin (composite)          */
    int      temperature_celsius;         /**< Converted to °C             */

    /* Wear and spare */
    uint8_t  available_spare;             /**< Percentage available (0-100) */
    uint8_t  available_spare_threshold;   /**< Warning threshold (%)       */
    uint8_t  percent_used;               /**< Estimated life used (%)     */

    /* I/O statistics (lower 64 bits of 128-bit fields) */
    uint64_t data_units_read;             /**< In 1000 × 512-byte units   */
    uint64_t data_units_written;          /**< In 1000 × 512-byte units   */
    uint64_t host_read_commands;          /**< Count                       */
    uint64_t host_write_commands;         /**< Count                       */
    uint64_t controller_busy_time;        /**< Minutes                     */

    /* Lifetime counters (lower 64 bits) */
    uint64_t power_cycles;
    uint64_t power_on_hours;
    uint64_t unsafe_shutdowns;
    uint64_t media_errors;
    uint64_t error_log_entries;

    /* Temperature thresholds */
    uint32_t warning_temp_time;           /**< Minutes over warning temp   */
    uint32_t critical_temp_time;          /**< Minutes over critical temp  */

    /* Temperature sensors (up to 8, in Kelvin, 0 = not implemented) */
    uint16_t temp_sensors[8];
};

/* =============================================================
 *  API Functions
 * ============================================================= */

/**
 * @brief Send Identify Controller and parse the result.
 * @param ctrl  Controller context (must be enabled)
 * @param id    Output: parsed controller identity
 * @return 0 on success, NVMe status field on error, -1 on timeout
 */
int nvme_identify_controller(struct nvme_ctrl *ctrl, struct nvme_id_ctrl *id);

/**
 * @brief Send Identify Namespace and parse the result.
 * @param ctrl  Controller context
 * @param nsid  Namespace ID (typically 1)
 * @param ns    Output: parsed namespace identity
 * @return 0 on success
 */
int nvme_identify_namespace(struct nvme_ctrl *ctrl, uint32_t nsid,
                            struct nvme_id_ns *ns);

/**
 * @brief Retrieve SMART / Health Information log.
 * @param ctrl   Controller context
 * @param nsid   Namespace ID (0xFFFFFFFF for global)
 * @param smart  Output: parsed SMART data
 * @return 0 on success
 *
 * Uses Get Log Page (opcode 0x02, LID=0x02).
 */
int nvme_get_smart_log(struct nvme_ctrl *ctrl, uint32_t nsid,
                       struct nvme_smart_log *smart);

/* =============================================================
 *  Pretty-print functions (for milestone3.c output)
 * ============================================================= */

void nvme_print_id_ctrl(const struct nvme_id_ctrl *id);
void nvme_print_id_ns(const struct nvme_id_ns *ns, uint32_t nsid);
void nvme_print_smart(const struct nvme_smart_log *smart);

#endif /* NVME_ADMIN_H */
