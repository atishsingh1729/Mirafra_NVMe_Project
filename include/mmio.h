#ifndef NVME_MMIO_H
#define NVME_MMIO_H
#include <stdint.h>

void     mmio_init(volatile void *bar_base);
uint32_t mmio_read32(uint32_t offset);
void     mmio_write32(uint32_t offset, uint32_t value);
uint64_t mmio_read64(uint32_t offset);
void     mmio_write64(uint32_t offset, uint64_t value);

static inline void mmio_barrier(void) {
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}
#endif
