# NVMe Firmware Diagnostic Project — Makefile
# ==============================================
# Platform: Raspberry Pi 5 (ARM64) + Geekworm X1001
#
# Usage:
#   make              — build current milestone (milestone1)
#   make milestone1   — build Milestone 1 explicitly
#   make all          — build all available milestones
#   make clean        — remove build artifacts
#   make VERBOSE=1    — show compiler commands

CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE
CFLAGS  += -I include
LDFLAGS  =

# Common source modules (shared across all milestones)
COMMON_SRC = src/log.c src/pci.c src/mmio.c src/dma.c src/nvme_ctrl.c src/nvme_admin.c
COMMON_OBJ = $(COMMON_SRC:.c=.o)

# Milestone targets
M1_SRC = src/milestone1.c
M1_BIN = milestone1

M2_SRC = src/milestone2.c
M2_BIN = milestone2

M3_SRC = src/milestone3.c
M3_BIN = milestone3

# Default target
.PHONY: default all clean

default: $(M3_BIN)

all: $(M1_BIN) $(M2_BIN) $(M3_BIN)

# ── Milestone 1: Bring-up and Discovery ──────────────────────
$(M1_BIN): src/log.o src/pci.o src/mmio.o src/dma.o $(M1_SRC:.c=.o)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $@  (run with: sudo ./$@)"

# ── Milestone 2: Firmware MMIO and DMA ───────────────────────
$(M2_BIN): $(COMMON_OBJ) $(M2_SRC:.c=.o)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $@  (run with: sudo ./$@)"

# ── Milestone 3: NVMe Admin Path ────────────────────────────
$(M3_BIN): $(COMMON_OBJ) $(M3_SRC:.c=.o)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $@  (run with: sudo ./$@)"

# ── Pattern rules ────────────────────────────────────────────
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Clean ────────────────────────────────────────────────────
clean:
	rm -f src/*.o $(M1_BIN) $(M2_BIN) $(M3_BIN)
	rm -f nvme_m1.log nvme_m2.log nvme_m3.log
	@echo "Clean."
