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
COMMON_SRC = src/log.c src/pci.c src/mmio.c src/dma.c
COMMON_OBJ = $(COMMON_SRC:.c=.o)

# Milestone targets
M1_SRC = src/milestone1.c
M1_BIN = milestone1

# Default target
.PHONY: default all clean

default: $(M1_BIN)

all: $(M1_BIN)

# ── Milestone 1: Bring-up and Discovery ──────────────────────
$(M1_BIN): $(COMMON_OBJ) $(M1_SRC:.c=.o)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $@  (run with: sudo ./$@)"

# ── Pattern rules ────────────────────────────────────────────
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Clean ────────────────────────────────────────────────────
clean:
	rm -f src/*.o $(M1_BIN)
	rm -f nvme_m1.log
	@echo "Clean."
