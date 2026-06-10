/**
 * @file nvme_regs.h
 * @brief NVMe (Non-Volatile Memory Express) register map, bit fields,
 *        command/completion structures, and opcode definitions.
 *
 * All offsets and fields from NVMe Base Specification 1.3c.
 *
 * Analogy: This file is the "dictionary" for speaking NVMe.
 * Every register, bit field, command word, and status code has
 * a named constant so the code reads like English, not hex.
 */

#ifndef NVME_REGS_H
#define NVME_REGS_H

#include <stdint.h>

/* =============================================================
 *  BAR0 (Base Address Register 0) REGISTER OFFSETS
 * ============================================================= */

#define NVME_REG_CAP        0x00    /**< Controller Capabilities (64-bit)       */
#define NVME_REG_VS         0x08    /**< Version (32-bit)                       */
#define NVME_REG_INTMS      0x0C    /**< Interrupt Mask Set (32-bit)            */
#define NVME_REG_INTMC      0x10    /**< Interrupt Mask Clear (32-bit)          */
#define NVME_REG_CC         0x14    /**< Controller Configuration (32-bit)      */
#define NVME_REG_CSTS       0x1C    /**< Controller Status (32-bit)             */
#define NVME_REG_NSSR       0x20    /**< NVM Subsystem Reset (32-bit)           */
#define NVME_REG_AQA        0x24    /**< Admin Queue Attributes (32-bit)        */
#define NVME_REG_ASQ        0x28    /**< Admin SQ Base Address (64-bit)         */
#define NVME_REG_ACQ        0x30    /**< Admin CQ Base Address (64-bit)         */
#define NVME_REG_CMBLOC     0x38    /**< CMB (Controller Memory Buffer) Location */
#define NVME_REG_CMBSZ      0x3C    /**< CMB Size                              */
#define NVME_REG_SQ0TDBL    0x1000  /**< Doorbell region start                 */

/* =============================================================
 *  CAP (Controller Capabilities) — 64-bit, offset 0x00
 * ============================================================= */

#define CAP_MQES(cap)       ((uint16_t)((cap) & 0xFFFF))           /**< Max Queue Entries Supported (0-based) */
#define CAP_CQR(cap)        (((cap) >> 16) & 0x1)                  /**< Contiguous Queues Required            */
#define CAP_AMS(cap)        (((cap) >> 17) & 0x7)                  /**< Arbitration Mechanism Supported       */
#define CAP_TO(cap)         (((cap) >> 24) & 0xFF)                 /**< Timeout (×500 ms units)               */
#define CAP_DSTRD(cap)      (((cap) >> 32) & 0xF)                  /**< Doorbell Stride (2^(2+DSTRD) bytes)   */
#define CAP_NSSRS(cap)      (((cap) >> 36) & 0x1)                  /**< NVM Subsystem Reset Supported         */
#define CAP_CSS(cap)        (((cap) >> 37) & 0xFF)                 /**< Command Sets Supported                */
#define CAP_BPS(cap)        (((cap) >> 45) & 0x1)                  /**< Boot Partition Support                */
#define CAP_MPSMIN(cap)     (((cap) >> 48) & 0xF)                  /**< Min Memory Page Size (2^(12+val))     */
#define CAP_MPSMAX(cap)     (((cap) >> 52) & 0xF)                  /**< Max Memory Page Size (2^(12+val))     */

/* =============================================================
 *  VS (Version) — 32-bit, offset 0x08
 * ============================================================= */

#define VS_MAJOR(vs)        (((vs) >> 16) & 0xFFFF)
#define VS_MINOR(vs)        (((vs) >> 8) & 0xFF)
#define VS_PATCH(vs)        ((vs) & 0xFF)

/* =============================================================
 *  CC (Controller Configuration) — 32-bit, offset 0x14
 * ============================================================= */

#define CC_EN               (1U << 0)       /**< Enable                                 */
#define CC_CSS_SHIFT        4               /**< Command Set Selected shift              */
#define CC_CSS_NVM          (0U << 4)       /**< NVM Command Set                        */
#define CC_MPS_SHIFT        7               /**< Memory Page Size shift                  */
#define CC_MPS_4K           (0U << 7)       /**< MPS = 4096 bytes (2^(12+0))            */
#define CC_AMS_SHIFT        11              /**< Arbitration Mechanism shift             */
#define CC_SHN_SHIFT        14              /**< Shutdown Notification shift             */
#define CC_SHN_NONE         (0U << 14)
#define CC_SHN_NORMAL       (1U << 14)
#define CC_SHN_ABRUPT       (2U << 14)
#define CC_IOSQES_SHIFT     16              /**< I/O SQ Entry Size shift (power of 2)   */
#define CC_IOSQES_64        (6U << 16)      /**< 2^6 = 64 bytes                         */
#define CC_IOCQES_SHIFT     20              /**< I/O CQ Entry Size shift (power of 2)   */
#define CC_IOCQES_16        (4U << 20)      /**< 2^4 = 16 bytes                         */

/* =============================================================
 *  CSTS (Controller Status) — 32-bit, offset 0x1C
 * ============================================================= */

#define CSTS_RDY            (1U << 0)       /**< Ready                                  */
#define CSTS_CFS            (1U << 1)       /**< Controller Fatal Status                */
#define CSTS_SHST_MASK      (3U << 2)       /**< Shutdown Status mask                   */
#define CSTS_SHST_NORMAL    (0U << 2)
#define CSTS_SHST_OCCURRING (1U << 2)
#define CSTS_SHST_COMPLETE  (2U << 2)
#define CSTS_NSSRO          (1U << 4)       /**< NVM Subsystem Reset Occurred           */
#define CSTS_PP             (1U << 5)       /**< Processing Paused                      */

/* =============================================================
 *  DOORBELL OFFSETS
 *
 *  SQ y Tail Doorbell = 0x1000 + (2y)     × (4 << CAP.DSTRD)
 *  CQ y Head Doorbell = 0x1000 + (2y + 1) × (4 << CAP.DSTRD)
 * ============================================================= */

static inline uint32_t nvme_sq_doorbell_offset(uint16_t qid, uint32_t dstrd)
{
    return 0x1000 + (2 * qid) * (4 << dstrd);
}

static inline uint32_t nvme_cq_doorbell_offset(uint16_t qid, uint32_t dstrd)
{
    return 0x1000 + (2 * qid + 1) * (4 << dstrd);
}

/* =============================================================
 *  NVMe COMMAND (SQE — Submission Queue Entry) — 64 bytes
 * ============================================================= */

struct nvme_cmd {
    /* CDW0 (Command Dword 0) */
    uint8_t  opcode;        /**< Command opcode                     */
    uint8_t  flags;         /**< Fused (bits 1:0), PSDT (bits 7:6)  */
    uint16_t cid;           /**< Command Identifier                 */

    /* CDW1 */
    uint32_t nsid;          /**< Namespace Identifier               */

    /* CDW2-3 */
    uint32_t cdw2;
    uint32_t cdw3;

    /* CDW4-5 — Metadata Pointer */
    uint64_t mptr;

    /* CDW6-9 — PRP (Physical Region Page) entries */
    uint64_t prp1;          /**< Data buffer address (bus address)  */
    uint64_t prp2;          /**< Second page or PRP list            */

    /* CDW10-15 — Command-specific */
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed));

_Static_assert(sizeof(struct nvme_cmd) == 64, "SQE must be 64 bytes");

/* =============================================================
 *  NVMe COMPLETION (CQE — Completion Queue Entry) — 16 bytes
 * ============================================================= */

struct nvme_cpl {
    uint32_t result;        /**< Command-specific result (DW0)      */
    uint32_t rsvd;          /**< Reserved (DW1)                     */
    uint16_t sq_head;       /**< SQ Head Pointer                    */
    uint16_t sq_id;         /**< SQ Identifier                      */
    uint16_t cid;           /**< Command Identifier                 */
    uint16_t status;        /**< Bit 0 = Phase, Bits 15:1 = SF      */
} __attribute__((packed));

_Static_assert(sizeof(struct nvme_cpl) == 16, "CQE must be 16 bytes");

/** Extract phase bit from CQE status word */
#define CQE_PHASE(s)        ((s) & 1)
/** Extract Status Field from CQE (SC + SCT + CRD + M + DNR) */
#define CQE_STATUS(s)       (((s) >> 1) & 0x7FFF)
/** Extract Status Code from Status Field */
#define CQE_SC(sf)          ((sf) & 0xFF)
/** Extract Status Code Type from Status Field */
#define CQE_SCT(sf)         (((sf) >> 8) & 0x7)

/* =============================================================
 *  ADMIN COMMAND OPCODES
 * ============================================================= */

#define NVME_ADMIN_DELETE_IO_SQ     0x00
#define NVME_ADMIN_CREATE_IO_SQ     0x01
#define NVME_ADMIN_GET_LOG_PAGE     0x02
#define NVME_ADMIN_DELETE_IO_CQ     0x04
#define NVME_ADMIN_CREATE_IO_CQ     0x05
#define NVME_ADMIN_IDENTIFY         0x06
#define NVME_ADMIN_ABORT            0x08
#define NVME_ADMIN_SET_FEATURES     0x09
#define NVME_ADMIN_GET_FEATURES     0x0A
#define NVME_ADMIN_ASYNC_EVENT      0x0C

/* =============================================================
 *  I/O COMMAND OPCODES (NVM Command Set)
 * ============================================================= */

#define NVME_IO_FLUSH               0x00
#define NVME_IO_WRITE               0x01
#define NVME_IO_READ                0x02

/* =============================================================
 *  SET FEATURES — Feature Identifiers
 * ============================================================= */

#define NVME_FEAT_NUM_QUEUES        0x07    /**< Number of Queues               */
#define NVME_FEAT_IRQ_COALESCE      0x08    /**< Interrupt Coalescing           */
#define NVME_FEAT_IRQ_CONFIG        0x09    /**< Interrupt Vector Configuration */

/* =============================================================
 *  IDENTIFY — CNS (Controller or Namespace Structure) values
 * ============================================================= */

#define NVME_IDENTIFY_NS            0x00    /**< Identify Namespace             */
#define NVME_IDENTIFY_CTRL          0x01    /**< Identify Controller            */
#define NVME_IDENTIFY_NS_LIST       0x02    /**< Active Namespace ID List       */

/* =============================================================
 *  GET LOG PAGE — Log Identifiers
 * ============================================================= */

#define NVME_LOG_ERROR              0x01    /**< Error Information              */
#define NVME_LOG_SMART              0x02    /**< SMART / Health Information     */
#define NVME_LOG_FW_SLOT            0x03    /**< Firmware Slot Information      */

#endif /* NVME_REGS_H */
