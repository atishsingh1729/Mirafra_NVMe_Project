/**
 * @file milestone1.c
 * @brief Milestone 1 — NVMe Bring-up and Discovery
 *
 * Deliverables:
 *   [M1.1] Locate NVMe under /sys/bus/pci by class code
 *   [M1.2] Open BAR0 (Base Address Register 0) and mmap into userspace
 *   [M1.3] Read CAP (Capabilities) and VS (Version), confirm device responds
 *   [M1.4] Dump the full register map as a baseline reference
 *   [M1.5] Set up a logger that timestamps every register read/write
 *
 * Build:  make
 * Run:    sudo ./milestone1
 * Log:    nvme_m1.log (created in the current directory)
 */

#include "log.h"
#include "pci.h"
#include "mmio.h"
#include "dma.h"
#include "nvme_regs.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* ── Register decoding helpers ─────────────────────────────── */

static void decode_cap(uint64_t cap)
{
    log_msg(LOG_INFO, "CAP (Controller Capabilities) = 0x%016llX",
            (unsigned long long)cap);
    log_msg(LOG_INFO, "  MQES  (Max Queue Entries)     : %u (supports %u entries)",
            CAP_MQES(cap), CAP_MQES(cap) + 1);
    log_msg(LOG_INFO, "  CQR   (Contiguous Queues Req) : %u", (unsigned)CAP_CQR(cap));

    uint8_t ams = CAP_AMS(cap);
    log_msg(LOG_INFO, "  AMS   (Arbitration Mechanisms) : 0x%X [%s%s]",
            ams,
            (ams & 1) ? "Weighted-RR " : "",
            (ams & 2) ? "Vendor-Specific" : "");

    uint8_t to = CAP_TO(cap);
    log_msg(LOG_INFO, "  TO    (Timeout)                : %u × 500 ms = %u ms", to, to * 500);

    uint8_t dstrd = CAP_DSTRD(cap);
    log_msg(LOG_INFO, "  DSTRD (Doorbell Stride)        : %u (stride = %u bytes)", dstrd, 4 << dstrd);

    log_msg(LOG_INFO, "  NSSRS (Subsystem Reset)        : %u", (unsigned)CAP_NSSRS(cap));

    uint8_t css = CAP_CSS(cap);
    log_msg(LOG_INFO, "  CSS   (Command Sets)           : 0x%02X [%s%s%s]",
            css,
            (css & 0x01) ? "NVM " : "",
            (css & 0x40) ? "Admin-Only " : "",
            (css & 0x80) ? "Multi-I/O" : "");

    log_msg(LOG_INFO, "  BPS   (Boot Partition)         : %u", (unsigned)CAP_BPS(cap));

    uint8_t mpsmin = CAP_MPSMIN(cap);
    uint8_t mpsmax = CAP_MPSMAX(cap);
    log_msg(LOG_INFO, "  MPSMIN (Min Page Size)         : %u → %u bytes", mpsmin, 1 << (12 + mpsmin));
    log_msg(LOG_INFO, "  MPSMAX (Max Page Size)         : %u → %u bytes", mpsmax, 1 << (12 + mpsmax));
}

static void decode_vs(uint32_t vs)
{
    log_msg(LOG_INFO, "VS (Version) = 0x%08X → NVMe %u.%u.%u",
            vs, VS_MAJOR(vs), VS_MINOR(vs), VS_PATCH(vs));
}

static void decode_cc(uint32_t cc)
{
    log_msg(LOG_INFO, "CC (Controller Configuration) = 0x%08X", cc);
    log_msg(LOG_INFO, "  EN     (Enable)              : %u", (cc & CC_EN) ? 1 : 0);
    log_msg(LOG_INFO, "  CSS    (Command Set)         : %u", (cc >> CC_CSS_SHIFT) & 0x7);

    uint8_t mps = (cc >> CC_MPS_SHIFT) & 0xF;
    log_msg(LOG_INFO, "  MPS    (Memory Page Size)    : %u → %u bytes", mps, 1 << (12 + mps));
    log_msg(LOG_INFO, "  AMS    (Arbitration)         : %u", (cc >> CC_AMS_SHIFT) & 0x7);

    uint8_t shn = (cc >> CC_SHN_SHIFT) & 0x3;
    log_msg(LOG_INFO, "  SHN    (Shutdown Notification): %u [%s]", shn,
            shn == 0 ? "None" : shn == 1 ? "Normal" : shn == 2 ? "Abrupt" : "Reserved");

    uint8_t iosqes = (cc >> CC_IOSQES_SHIFT) & 0xF;
    uint8_t iocqes = (cc >> CC_IOCQES_SHIFT) & 0xF;
    log_msg(LOG_INFO, "  IOSQES (I/O SQ Entry Size)   : %u → %u bytes", iosqes, 1 << iosqes);
    log_msg(LOG_INFO, "  IOCQES (I/O CQ Entry Size)   : %u → %u bytes", iocqes, 1 << iocqes);
}

static void decode_csts(uint32_t csts)
{
    log_msg(LOG_INFO, "CSTS (Controller Status) = 0x%08X", csts);
    log_msg(LOG_INFO, "  RDY    (Ready)               : %u", (csts & CSTS_RDY) ? 1 : 0);
    log_msg(LOG_INFO, "  CFS    (Controller Fatal)    : %u%s",
            (csts & CSTS_CFS) ? 1 : 0,
            (csts & CSTS_CFS) ? "  *** FATAL ***" : "");

    uint8_t shst = (csts & CSTS_SHST_MASK) >> 2;
    log_msg(LOG_INFO, "  SHST   (Shutdown Status)     : %u [%s]", shst,
            shst == 0 ? "Normal" : shst == 1 ? "Processing" : shst == 2 ? "Complete" : "Reserved");
    log_msg(LOG_INFO, "  NSSRO  (Subsystem Reset Occ) : %u", (csts & CSTS_NSSRO) ? 1 : 0);
    log_msg(LOG_INFO, "  PP     (Processing Paused)   : %u", (csts & CSTS_PP) ? 1 : 0);
}

static void decode_aqa(uint32_t aqa)
{
    log_msg(LOG_INFO, "AQA (Admin Queue Attributes) = 0x%08X", aqa);
    log_msg(LOG_INFO, "  ASQS (Admin SQ Size) : %u entries", (aqa & 0xFFF) + 1);
    log_msg(LOG_INFO, "  ACQS (Admin CQ Size) : %u entries", ((aqa >> 16) & 0xFFF) + 1);
}

/* ── Doorbell region scan ──────────────────────────────────── */

static void dump_doorbell_region(uint32_t dstrd)
{
    /* Read admin queue doorbells (QID = 0) and first I/O queue (QID = 1) */
    log_msg(LOG_INFO, "Doorbell region (first 2 queue pairs):");
    for (int qid = 0; qid < 2; qid++) {
        uint32_t sq_off = nvme_sq_doorbell_offset(qid, dstrd);
        uint32_t cq_off = nvme_cq_doorbell_offset(qid, dstrd);
        uint32_t sq_val = mmio_read32(sq_off);
        uint32_t cq_val = mmio_read32(cq_off);
        log_msg(LOG_INFO, "  QID %d: SQ Tail DB [0x%04X] = 0x%08X  |  CQ Head DB [0x%04X] = 0x%08X",
                qid, sq_off, sq_val, cq_off, cq_val);
    }
}

/* ── Raw hex dump of register space ───────────────────────── */

static void dump_raw_registers(void)
{
    log_msg(LOG_INFO, "Raw register dump (0x00–0x3F, controller registers):");
    for (uint32_t off = 0; off <= 0x3C; off += 4) {
        uint32_t val = mmio_read32(off);
        log_msg(LOG_INFO, "  [0x%04X] = 0x%08X", off, val);
    }
}

/* =============================================================
 *  MAIN
 * ============================================================= */

int main(int argc, char *argv[])
{
    int rc = 0;
    struct pci_device dev;

    /* ── [M1.5] Initialise timestamped logger ─────────────────
     *
     * LOG_TRACE captures every MMIO read/write.
     * Use LOG_INFO for less verbose output.
     * All output goes to both stdout and nvme_m1.log.           */

    const char *log_file = "nvme_m1.log";
    log_level_t level = LOG_TRACE;

    /* Allow overriding log level via command line */
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'q')
        level = LOG_INFO;  /* -q = quiet, skip TRACE */

    log_init(level, log_file);

    log_msg(LOG_INFO, "═══════════════════════════════════════════════");
    log_msg(LOG_INFO, "  Milestone 1 — NVMe Bring-up and Discovery");
    log_msg(LOG_INFO, "  NVMe Firmware Diagnostic Project");
    log_msg(LOG_INFO, "  Platform: Raspberry Pi 5 + Geekworm X1001");
    log_msg(LOG_INFO, "═══════════════════════════════════════════════");
    log_msg(LOG_INFO, "System page size: %ld bytes", sysconf(_SC_PAGESIZE));
    log_msg(LOG_INFO, "Log file: %s", log_file);

    /* ── [M1.1] Locate NVMe device by class code ─────────────
     *
     * Scans /sys/bus/pci/devices for PCI class 0x010802
     * (Mass Storage → NVM → NVMe).                             */

    log_msg(LOG_INFO, " ");
    log_msg(LOG_INFO, "[M1.1] Scanning PCIe bus for NVMe devices...");

    if (pci_find_nvme(&dev) < 0) {
        log_msg(LOG_ERROR, "No NVMe device found. Is the Geekworm X1001 seated?");
        rc = 1;
        goto done;
    }

    /* Detect PCIe DMA inbound offset (needed for Milestone 2+) */
    pci_detect_dma_offset(&dev);

    /* ── [M1.2] Open BAR0 and mmap into userspace ────────────
     *
     * BAR0 is the NVMe controller's register window.
     * We mmap via sysfs resource0 (handles PCIe address
     * translation automatically).
     *
     * NOTE: We do NOT unbind the kernel driver for Milestone 1.
     * We only READ registers — no writes except to doorbells
     * (which we skip). The kernel driver coexists safely for
     * read-only discovery.                                      */

    log_msg(LOG_INFO, " ");
    log_msg(LOG_INFO, "[M1.2] Mapping BAR0 into userspace...");

    if (pci_map_bar0(&dev) < 0) {
        log_msg(LOG_ERROR, "Failed to map BAR0. Run as root (sudo).");
        rc = 1;
        goto done;
    }

    /* Initialise MMIO subsystem with the BAR0 pointer */
    mmio_init(dev.bar0);

    /* ── [M1.3] Read CAP and VS, confirm device responds ─────
     *
     * CAP (Controller Capabilities) tells us the controller's
     * limits: max queue depth, timeout, doorbell stride, etc.
     * VS (Version) tells us the NVMe spec version.
     *
     * If these return 0xFFFFFFFF, the PCIe link is down.        */

    log_msg(LOG_INFO, " ");
    log_msg(LOG_INFO, "[M1.3] Reading controller identity registers...");

    uint64_t cap  = mmio_read64(NVME_REG_CAP);
    uint32_t vs   = mmio_read32(NVME_REG_VS);

    /* Sanity check: 0xFFFFFFFF means device is unreachable */
    if (vs == 0xFFFFFFFF || (cap & 0xFFFFFFFF) == 0xFFFFFFFF) {
        log_msg(LOG_ERROR, "Device reads 0xFFFFFFFF — PCIe link is DOWN!");
        log_msg(LOG_ERROR, "Check physical connection and power.");
        rc = 1;
        goto cleanup;
    }

    log_msg(LOG_INFO, "Device is responsive. Decoding registers...");
    log_msg(LOG_INFO, " ");

    /* Decode CAP (Capabilities) — the most important register */
    decode_cap(cap);
    log_msg(LOG_INFO, " ");

    /* Decode VS (Version) */
    decode_vs(vs);
    log_msg(LOG_INFO, " ");

    /* ── [M1.4] Dump the full register map ───────────────────
     *
     * Read and decode every controller register. This serves
     * as the baseline reference for subsequent milestones.
     *
     * Registers read:
     *   CAP, VS, INTMS, INTMC, CC, CSTS, NSSR, AQA, ASQ, ACQ,
     *   CMBLOC, CMBSZ, and doorbell region.                    */

    log_msg(LOG_INFO, "[M1.4] Full register map dump...");
    log_msg(LOG_INFO, " ");

    /* CC (Controller Configuration) */
    uint32_t cc = mmio_read32(NVME_REG_CC);
    decode_cc(cc);
    log_msg(LOG_INFO, " ");

    /* CSTS (Controller Status) */
    uint32_t csts = mmio_read32(NVME_REG_CSTS);
    decode_csts(csts);
    log_msg(LOG_INFO, " ");

    /* Interrupt mask registers */
    uint32_t intms = mmio_read32(NVME_REG_INTMS);
    uint32_t intmc = mmio_read32(NVME_REG_INTMC);
    log_msg(LOG_INFO, "INTMS (Interrupt Mask Set)   = 0x%08X", intms);
    log_msg(LOG_INFO, "INTMC (Interrupt Mask Clear) = 0x%08X", intmc);
    log_msg(LOG_INFO, " ");

    /* NSSR (NVM Subsystem Reset) */
    uint32_t nssr = mmio_read32(NVME_REG_NSSR);
    log_msg(LOG_INFO, "NSSR (NVM Subsystem Reset) = 0x%08X", nssr);
    log_msg(LOG_INFO, " ");

    /* AQA, ASQ, ACQ (Admin Queue config — set by kernel driver) */
    uint32_t aqa = mmio_read32(NVME_REG_AQA);
    uint64_t asq = mmio_read64(NVME_REG_ASQ);
    uint64_t acq = mmio_read64(NVME_REG_ACQ);
    decode_aqa(aqa);
    log_msg(LOG_INFO, "ASQ (Admin SQ Base) = 0x%016llX", (unsigned long long)asq);
    log_msg(LOG_INFO, "ACQ (Admin CQ Base) = 0x%016llX", (unsigned long long)acq);
    log_msg(LOG_INFO, " ");

    /* CMB (Controller Memory Buffer) — optional feature */
    uint32_t cmbloc = mmio_read32(NVME_REG_CMBLOC);
    uint32_t cmbsz  = mmio_read32(NVME_REG_CMBSZ);
    log_msg(LOG_INFO, "CMBLOC (CMB Location) = 0x%08X", cmbloc);
    log_msg(LOG_INFO, "CMBSZ  (CMB Size)     = 0x%08X  [%s]",
            cmbsz, cmbsz ? "CMB supported" : "No CMB");
    log_msg(LOG_INFO, " ");

    /* Doorbell region */
    uint32_t dstrd = CAP_DSTRD(cap);
    dump_doorbell_region(dstrd);
    log_msg(LOG_INFO, " ");

    /* Raw hex dump for reference */
    dump_raw_registers();
    log_msg(LOG_INFO, " ");

    /* ── Summary ──────────────────────────────────────────── */

    log_msg(LOG_INFO, "═══════════════════════════════════════════════");
    log_msg(LOG_INFO, "  Milestone 1 — Summary");
    log_msg(LOG_INFO, "═══════════════════════════════════════════════");
    log_msg(LOG_INFO, "  Device       : %s (VID=0x%04X DID=0x%04X)",
            dev.bdf, dev.vendor_id, dev.device_id);
    log_msg(LOG_INFO, "  NVMe version : %u.%u.%u", VS_MAJOR(vs), VS_MINOR(vs), VS_PATCH(vs));
    log_msg(LOG_INFO, "  Max queues   : %u entries", CAP_MQES(cap) + 1);
    log_msg(LOG_INFO, "  Page size    : %u – %u bytes",
            1 << (12 + CAP_MPSMIN(cap)), 1 << (12 + CAP_MPSMAX(cap)));
    log_msg(LOG_INFO, "  Timeout      : %u ms", (unsigned)(CAP_TO(cap) * 500));
    log_msg(LOG_INFO, "  DB stride    : %u bytes", 4 << dstrd);
    log_msg(LOG_INFO, "  DMA offset   : 0x%llX", (unsigned long long)dev.dma_offset);
    log_msg(LOG_INFO, "  Controller   : %s",
            (cc & CC_EN) ? "ENABLED (kernel driver active)" : "DISABLED");
    log_msg(LOG_INFO, "  Status       : %s",
            (csts & CSTS_RDY) ? "READY" : "NOT READY");
    log_msg(LOG_INFO, " ");
    log_msg(LOG_INFO, "  [M1.1] ✓ Device located by PCI class code");
    log_msg(LOG_INFO, "  [M1.2] ✓ BAR0 mapped at %p", dev.bar0);
    log_msg(LOG_INFO, "  [M1.3] ✓ CAP/VS read, device responsive");
    log_msg(LOG_INFO, "  [M1.4] ✓ Full register map dumped");
    log_msg(LOG_INFO, "  [M1.5] ✓ Logger active → %s", log_file);
    log_msg(LOG_INFO, " ");
    log_msg(LOG_INFO, "  Ready for Milestone 2 (DMA buffer setup).");

cleanup:
    pci_unmap_bar0(&dev);

done:
    log_shutdown();
    return rc;
}
