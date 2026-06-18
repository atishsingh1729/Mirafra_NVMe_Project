obj-m := nvme_irq.o
nvme_irq-objs := nvme_irq_mod.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	sudo insmod nvme_irq.ko

uninstall:
	sudo rmmod nvme_irq
