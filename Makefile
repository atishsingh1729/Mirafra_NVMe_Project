CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -I include
LDFLAGS  =

COMMON_SRC = src/log.c src/pci.c src/mmio.c src/dma.c src/nvme_ctrl.c src/nvme_admin.c src/nvme_queue.c src/nvme_irq.c
COMMON_OBJ = $(COMMON_SRC:.c=.o)

BINS = milestone1 milestone2 milestone3 milestone4 milestone5

.PHONY: default all clean

default: milestone5

all: $(BINS)

milestone1: src/log.o src/pci.o src/mmio.o src/dma.o src/milestone1.o
	$(CC) $(CFLAGS) -o $@ $^

milestone2 milestone3: %: $(COMMON_OBJ) src/%.o
	$(CC) $(CFLAGS) -o $@ $^

milestone4 milestone5: %: $(COMMON_OBJ) src/%.o
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o $(BINS) nvme_m*.log
