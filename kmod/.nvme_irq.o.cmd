savedcmd_nvme_irq.o := aarch64-linux-gnu-ld -EL  -maarch64elf -z noexecstack --no-warn-rwx-segments   -r -o nvme_irq.o @nvme_irq.mod 
