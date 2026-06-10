/**
 * @file dma.h
 * @brief DMA (Direct Memory Access) buffer allocation, physical
 *        address lookup, and ARM64 cache maintenance.
 *
 * On BCM2712 (RPi 5), PCIe DMA is NOT cache-coherent. Every
 * buffer shared between CPU and NVMe controller needs explicit
 * cache maintenance:
 *   - dc cvac  (flush CPU cache → RAM) before device reads it
 *   - dc civac (clean+invalidate) before CPU reads DMA'd data
 *
 * Analogy: The CPU and NVMe controller share lockers (RAM).
 * The CPU has a notepad (cache) that might be out of date.
 * Flush = update the locker from the notepad.
 * Invalidate = throw away the notepad, read the locker fresh.
 */

#ifndef NVME_DMA_H
#define NVME_DMA_H

#include <stdint.h>
#include <stddef.h>

/** A single DMA-able buffer with tracked addresses */
struct dma_buffer {
    void     *virt;         /**< CPU virtual address (for our code)     */
    uint64_t  phys;         /**< CPU physical address (from pagemap)    */
    uint64_t  bus;          /**< PCIe bus address (phys + dma_offset)   */
    size_t    size;         /**< Buffer size in bytes                   */
};

/**
 * @brief Allocate a page-aligned, pinned DMA buffer.
 * @param buf         Output buffer descriptor
 * @param size        Requested size (rounded up to page size)
 * @param dma_offset  PCIe inbound DMA offset to compute bus address
 * @return 0 on success, -1 on failure
 *
 * The buffer is zeroed, flushed to RAM (cache lines clean),
 * and mlock'd so it won't be swapped during DMA.
 */
int dma_alloc(struct dma_buffer *buf, size_t size, uint64_t dma_offset);

/** Free a DMA buffer allocated by dma_alloc. */
void dma_free(struct dma_buffer *buf);

/**
 * @brief Flush region from CPU cache to RAM.
 * @param addr  Virtual address (start)
 * @param len   Number of bytes to flush
 *
 * Use AFTER writing to a buffer that the NVMe controller will
 * DMA-read (SQ entries, write data buffers).
 */
void dma_flush(void *addr, size_t len);

/**
 * @brief Invalidate region in CPU cache.
 * @param addr  Virtual address (start)
 * @param len   Number of bytes to invalidate
 *
 * Use BEFORE reading a buffer that the NVMe controller DMA-wrote
 * (CQ entries, read data buffers, identify data).
 */
void dma_invalidate(void *addr, size_t len);

/**
 * @brief Look up the CPU physical address of a virtual address.
 * @param virt  Virtual address to translate
 * @return Physical address, or 0 on failure
 *
 * Uses /proc/self/pagemap. Respects the system page size
 * (4K/16K/64K on ARM64).
 */
uint64_t dma_virt_to_phys(void *virt);

#endif /* NVME_DMA_H */
