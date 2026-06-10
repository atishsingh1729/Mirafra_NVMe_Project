/**
 * @file mmio.c
 * @brief MMIO access with automatic logging of every register touch.
 */

#include "mmio.h"
#include "log.h"
#include <stdint.h>

static volatile void *g_bar_base;

void mmio_init(volatile void *bar_base)
{
    g_bar_base = bar_base;
}

uint32_t mmio_read32(uint32_t offset)
{
    mmio_barrier();
    uint32_t val = *(volatile uint32_t *)((uint8_t *)g_bar_base + offset);
    mmio_barrier();
    log_mmio_read(offset, 32, val);
    return val;
}

void mmio_write32(uint32_t offset, uint32_t value)
{
    log_mmio_write(offset, 32, value);
    mmio_barrier();
    *(volatile uint32_t *)((uint8_t *)g_bar_base + offset) = value;
    mmio_barrier();
}

uint64_t mmio_read64(uint32_t offset)
{
    /* NVMe spec permits 64-bit reads as two 32-bit reads (lo first) */
    mmio_barrier();
    uint32_t lo = *(volatile uint32_t *)((uint8_t *)g_bar_base + offset);
    uint32_t hi = *(volatile uint32_t *)((uint8_t *)g_bar_base + offset + 4);
    mmio_barrier();
    uint64_t val = ((uint64_t)hi << 32) | lo;
    log_mmio_read(offset, 64, val);
    return val;
}

void mmio_write64(uint32_t offset, uint64_t value)
{
    log_mmio_write(offset, 64, value);
    mmio_barrier();
    *(volatile uint32_t *)((uint8_t *)g_bar_base + offset) = (uint32_t)(value & 0xFFFFFFFF);
    *(volatile uint32_t *)((uint8_t *)g_bar_base + offset + 4) = (uint32_t)(value >> 32);
    mmio_barrier();
}
