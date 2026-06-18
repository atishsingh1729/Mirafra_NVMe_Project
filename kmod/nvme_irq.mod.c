#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xc1514a3b, "free_irq" },
	{ 0x83c7c207, "misc_deregister" },
	{ 0xb8fe0b43, "pci_enable_device" },
	{ 0x941f2aaa, "eventfd_ctx_put" },
	{ 0x281bfe91, "pci_alloc_irq_vectors" },
	{ 0x69bcf927, "pci_dev_put" },
	{ 0x9222bbfc, "pci_set_power_state" },
	{ 0xae7886f8, "pci_irq_vector" },
	{ 0x92997ed8, "_printk" },
	{ 0x92d5838e, "request_threaded_irq" },
	{ 0xd67364f7, "eventfd_ctx_fdget" },
	{ 0x89c91ed8, "misc_register" },
	{ 0xf05f66a2, "pci_set_master" },
	{ 0x6d381ebb, "pci_get_class" },
	{ 0xa743933d, "pci_disable_device" },
	{ 0x0c10e997, "eventfd_signal_mask" },
	{ 0xa8c09886, "pci_free_irq_vectors" },
	{ 0x91d66ee9, "module_layout" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "C087AE0CFD86BDE880F9C44");
