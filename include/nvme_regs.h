#ifndef NVME_REGS_H
#define NVME_REGS_H
#include <stdint.h>

/* BAR0 register offsets */
#define NVME_REG_CAP    0x00
#define NVME_REG_VS     0x08
#define NVME_REG_INTMS  0x0C
#define NVME_REG_INTMC  0x10
#define NVME_REG_CC     0x14
#define NVME_REG_CSTS   0x1C
#define NVME_REG_NSSR   0x20
#define NVME_REG_AQA    0x24
#define NVME_REG_ASQ    0x28
#define NVME_REG_ACQ    0x30

/* CAP field extraction */
#define CAP_MQES(c)     ((uint16_t)((c) & 0xFFFF))
#define CAP_CQR(c)      (((c) >> 16) & 1)
#define CAP_TO(c)       (((c) >> 24) & 0xFF)
#define CAP_DSTRD(c)    (((c) >> 32) & 0xF)
#define CAP_CSS(c)      (((c) >> 37) & 0xFF)
#define CAP_MPSMIN(c)   (((c) >> 48) & 0xF)
#define CAP_MPSMAX(c)   (((c) >> 52) & 0xF)

/* VS field extraction */
#define VS_MAJOR(v)     (((v) >> 16) & 0xFFFF)
#define VS_MINOR(v)     (((v) >> 8) & 0xFF)

/* CC bits */
#define CC_EN           (1U << 0)
#define CC_CSS_NVM      (0U << 4)
#define CC_MPS_4K       (0U << 7)
#define CC_IOSQES_64    (6U << 16)
#define CC_IOCQES_16    (4U << 20)

/* CSTS bits */
#define CSTS_RDY        (1U << 0)
#define CSTS_CFS        (1U << 1)

/* Doorbell offset calculation */
static inline uint32_t nvme_sq_db(uint16_t qid, uint32_t dstrd) {
    return 0x1000 + (2 * qid) * (4 << dstrd);
}
static inline uint32_t nvme_cq_db(uint16_t qid, uint32_t dstrd) {
    return 0x1000 + (2 * qid + 1) * (4 << dstrd);
}

/* SQE — 64 bytes */
struct nvme_cmd {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint32_t cdw2, cdw3;
    uint64_t mptr;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed));

/* CQE — 16 bytes */
struct nvme_cpl {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head, sq_id;
    uint16_t cid;
    uint16_t status;   /* bit 0 = phase, bits 15:1 = status field */
} __attribute__((packed));

#define CQE_PHASE(s)    ((s) & 1)
#define CQE_STATUS(s)   (((s) >> 1) & 0x7FFF)

/* Admin opcodes */
#define NVME_ADMIN_CREATE_IO_SQ  0x01
#define NVME_ADMIN_CREATE_IO_CQ  0x05
#define NVME_ADMIN_IDENTIFY      0x06
#define NVME_ADMIN_SET_FEATURES  0x09
#define NVME_ADMIN_GET_LOG_PAGE  0x02

/* I/O opcodes */
#define NVME_IO_WRITE   0x01
#define NVME_IO_READ    0x02

/* Feature IDs */
#define NVME_FEAT_NUM_QUEUES 0x07

/* Identify CNS values */
#define NVME_IDENTIFY_NS    0x00
#define NVME_IDENTIFY_CTRL  0x01

/* Log IDs */
#define NVME_LOG_SMART  0x02

#endif
