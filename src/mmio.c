#include "mmio.h"
#include "log.h"

static volatile void *g_bar;

void mmio_init(volatile void *base) { g_bar = base; }

uint32_t mmio_read32(uint32_t off) {
    mmio_barrier();
    uint32_t v = *(volatile uint32_t *)((uint8_t *)g_bar + off);
    mmio_barrier();
    log_mmio_read(off, 32, v);
    return v;
}

void mmio_write32(uint32_t off, uint32_t val) {
    log_mmio_write(off, 32, val);
    mmio_barrier();
    *(volatile uint32_t *)((uint8_t *)g_bar + off) = val;
    mmio_barrier();
}

uint64_t mmio_read64(uint32_t off) {
    mmio_barrier();
    uint32_t lo = *(volatile uint32_t *)((uint8_t *)g_bar + off);
    uint32_t hi = *(volatile uint32_t *)((uint8_t *)g_bar + off + 4);
    mmio_barrier();
    uint64_t v = ((uint64_t)hi << 32) | lo;
    log_mmio_read(off, 64, v);
    return v;
}

void mmio_write64(uint32_t off, uint64_t val) {
    log_mmio_write(off, 64, val);
    mmio_barrier();
    *(volatile uint32_t *)((uint8_t *)g_bar + off) = (uint32_t)val;
    *(volatile uint32_t *)((uint8_t *)g_bar + off + 4) = (uint32_t)(val >> 32);
    mmio_barrier();
}
