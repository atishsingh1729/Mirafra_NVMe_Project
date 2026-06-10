/**
 * @file mmio.h
 * @brief MMIO (Memory-Mapped I/O) access wrappers with integrated
 *        logging and ARM64 memory barriers.
 *
 * Every register read/write goes through these functions so the
 * logger captures a complete trace. ARM64 DSB (Data Synchronization
 * Barrier) instructions ensure ordering across the PCIe bus.
 *
 * Analogy: These wrappers are security cameras on every register
 * door — nobody reads or writes without being recorded.
 */

#ifndef NVME_MMIO_H
#define NVME_MMIO_H

#include <stdint.h>

/**
 * @brief Initialise MMIO subsystem with a BAR base pointer.
 * @param bar_base  mmap'd BAR0 pointer
 *
 * Must be called before any mmio_read/write functions.
 */
void mmio_init(volatile void *bar_base);

/** Read a 32-bit register at the given offset from BAR0. */
uint32_t mmio_read32(uint32_t offset);

/** Write a 32-bit value to the register at the given offset. */
void mmio_write32(uint32_t offset, uint32_t value);

/** Read a 64-bit register (two 32-bit reads, low word first). */
uint64_t mmio_read64(uint32_t offset);

/** Write a 64-bit value (two 32-bit writes, low word first). */
void mmio_write64(uint32_t offset, uint64_t value);

/** ARM64 full-system data synchronization barrier. */
static inline void mmio_barrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}

#endif /* NVME_MMIO_H */
