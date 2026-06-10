# NVMe Firmware Diagnostic and Transport-Aware Queue Manager

**Platform:** Raspberry Pi 5 (BCM2712) + Geekworm X1001 PCIe M.2 Adapter  
**Duration:** 10 working days (Mon 8 Jun – Fri 19 Jun 2026)

## Project Structure

```
nvme_fw/
├── include/
│   ├── nvme_regs.h     NVMe register map, bit fields, command structs
│   ├── pci.h           PCIe device discovery and BAR mapping
│   ├── mmio.h          MMIO access wrappers with logging
│   ├── dma.h           DMA buffer allocation and cache maintenance
│   └── (future)
│       ├── nvme_admin.h    Admin queue and commands (M3)
│       ├── nvme_queue.h    Queue engine with CID tracking (M4)
│       ├── nvme_irq.h      MSI-X interrupt handling (M5)
│       └── transport.h     Transport hooks and request struct (M6)
├── src/
│   ├── log.c           Timestamped logger (every MMIO access traced)
│   ├── pci.c           Sysfs scan, BAR mmap, DMA offset detection
│   ├── mmio.c          Register access with logging
│   ├── dma.c           Page alloc, mlock, virt-to-phys, cache ops
│   ├── milestone1.c    M1: discovery and register dump
│   └── (future)
│       ├── milestone2.c    M2: DMA buffer validation
│       ├── nvme_admin.c    M3: admin path (identify, SMART)
│       ├── nvme_queue.c    M4: ring buffer engine
│       ├── nvme_irq.c      M5: MSI-X or polling fallback
│       ├── transport.c     M6: transport hooks
│       ├── milestone7.c    M7: I/O flow simulation
│       ├── diag.c          M8: diagnostics and error injection
│       └── milestone9.c    M9: validation harness
├── Makefile
└── README.md
```

## Key Lessons (from prototype phase)

1. **PCIe DMA offset:** BCM2712 adds 0x1000000000 to CPU physical addresses
   for PCIe inbound (device→RAM) DMA. bus_addr = phys + offset.

2. **Cache coherence:** BCM2712 PCIe is NOT cache-coherent.
   - `dc cvac` (flush) after writing SQ entries / data buffers
   - `dc civac` (clean+invalidate) before reading CQ entries / DMA'd data
   - CRITICAL: flush pages after zeroing (before DMA) to avoid dirty writeback

3. **Set Features (Number of Queues):** Samsung PM991a requires this before
   Create I/O CQ/SQ. Always request ≥8 queues.

4. **CDW10 bit layout:** QID is bits [15:0], QSIZE is bits [31:16].

5. **16K page size:** RPi OS on BCM2712 uses 16K pages. All DMA buffers
   fit in single pages. Use sysconf(_SC_PAGESIZE) at runtime.

## Interrupt Architecture Plan (Milestone 5)

### Option A: Kernel Module + eventfd (Recommended)

Since VFIO is unavailable on this kernel, MSI-X from userspace requires
a thin kernel module:

```
┌─────────────────────┐     ┌──────────────────────┐
│   Userspace App     │     │   Kernel Module       │
│                     │     │   (nvme_irq.ko)       │
│  epoll(eventfd) ◄───┼─────┤  ISR writes eventfd   │
│  process CQ         │     │  request_irq(MSI-X)  │
│                     │     │                      │
│  ioctl → config     │────▶│  setup/teardown      │
└─────────────────────┘     └──────────────────────┘
         │                            │
         │         PCIe               │
         └───── BAR0 mmap ──────────▶ NVMe Controller
                                     (MSI-X vector → CPU IRQ)
```

The module:
- Allocates MSI-X vectors via `pci_alloc_irq_vectors()`
- Registers an ISR that writes to an `eventfd`
- Exposes the eventfd to userspace via ioctl or char device
- Userspace `epoll()` on the eventfd for CQ notifications

### Option B: Polling Fallback (Current implementation)

No kernel module needed. Poll CQ phase bit in a loop with
`usleep(100)`. Lower complexity, higher latency.

### Hybrid Strategy

The queue engine (M4) will abstract the notification mechanism:
```c
typedef void (*cq_notify_fn)(int qid, void *ctx);

struct nvme_queue_cfg {
    int  mode;          // NVME_NOTIFY_POLL or NVME_NOTIFY_IRQ
    int  irq_vector;    // MSI-X vector (if IRQ mode)
    int  eventfd;       // eventfd descriptor (if IRQ mode)
    cq_notify_fn notify;
};
```

This way Milestones 1–4 work with polling, and Milestone 5
adds interrupt support without changing the queue engine API.

## Build & Run

```bash
make
sudo ./milestone1        # Full MMIO trace
sudo ./milestone1 -q     # Quiet mode (INFO only, no MMIO trace)
```

Log output goes to `nvme_m1.log` in the current directory.
