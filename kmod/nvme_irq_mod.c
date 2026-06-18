/*
 * nvme_irq_mod.c — NVMe MSI-X to eventfd bridge
 *
 * Allocates an MSI-X vector on the NVMe device and signals
 * a userspace eventfd on every interrupt.
 *
 * Load sequence:
 *   echo on > /sys/bus/pci/devices/XXXX:XX:XX.X/power/control
 *   echo XXXX:XX:XX.X | tee /sys/bus/pci/drivers/nvme/unbind
 *   insmod nvme_irq.ko
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/eventfd.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>

#define NVME_IRQ_MAGIC 'N'
#define NVME_IRQ_SET_EVENTFD  _IOW(NVME_IRQ_MAGIC, 1, int)
#define NVME_IRQ_GET_INFO     _IOR(NVME_IRQ_MAGIC, 2, int)

static struct pci_dev      *g_pdev;
static struct eventfd_ctx  *g_efd;
static int                  g_irq;

static irqreturn_t nvme_msix_isr(int irq, void *data)
{
    if (g_efd)
        eventfd_signal(g_efd);
    return IRQ_HANDLED;
}

static long nvme_irq_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    if (cmd == NVME_IRQ_SET_EVENTFD) {
        int fd = (int)arg;
        if (g_efd) eventfd_ctx_put(g_efd);
        g_efd = eventfd_ctx_fdget(fd);
        if (IS_ERR(g_efd)) { g_efd = NULL; return -EINVAL; }
        pr_info("nvme_irq: eventfd registered (fd=%d)\n", fd);
        return 0;
    }
    if (cmd == NVME_IRQ_GET_INFO)
        return put_user(g_irq, (int __user *)arg);
    return -ENOTTY;
}

static const struct file_operations nvme_irq_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = nvme_irq_ioctl,
};

static struct miscdevice nvme_irq_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "nvme_irq",
    .fops  = &nvme_irq_fops,
};

static int __init nvme_irq_mod_init(void)
{
    int ret, nvec;

    /* Find NVMe by full 24-bit class: 01=storage, 08=NVM, 02=NVMe */
    g_pdev = pci_get_class(0x010802, NULL);
    if (!g_pdev) {
        pr_err("nvme_irq: no NVMe device found\n");
        return -ENODEV;
    }

    pr_info("nvme_irq: found %s [%04x:%04x]\n",
            pci_name(g_pdev), g_pdev->vendor, g_pdev->device);

    /* Wake device from any low-power state */
    ret = pci_set_power_state(g_pdev, PCI_D0);
    if (ret)
        pr_warn("nvme_irq: pci_set_power_state D0 returned %d (continuing)\n", ret);

    /* Enable the PCI device */
    ret = pci_enable_device(g_pdev);
    if (ret) {
        pr_err("nvme_irq: pci_enable_device failed (%d)\n", ret);
        goto err_put;
    }
    pci_set_master(g_pdev);

    /* Allocate MSI-X or MSI vector */
    nvec = pci_alloc_irq_vectors(g_pdev, 1, 1, PCI_IRQ_MSIX | PCI_IRQ_MSI);
    if (nvec < 0) {
        pr_err("nvme_irq: pci_alloc_irq_vectors failed (%d)\n", nvec);
        pr_err("nvme_irq: device may be in D3cold — try:\n");
        pr_err("nvme_irq:   echo on > /sys/.../power/control  BEFORE unbinding\n");
        ret = nvec;
        goto err_disable;
    }

    g_irq = pci_irq_vector(g_pdev, 0);
    ret = request_irq(g_irq, nvme_msix_isr, 0, "nvme_irq", NULL);
    if (ret) {
        pr_err("nvme_irq: request_irq(%d) failed (%d)\n", g_irq, ret);
        goto err_free_vec;
    }

    ret = misc_register(&nvme_irq_miscdev);
    if (ret) goto err_free_irq;

    pr_info("nvme_irq: loaded — IRQ=%d, /dev/nvme_irq ready\n", g_irq);
    return 0;

err_free_irq:  free_irq(g_irq, NULL);
err_free_vec:  pci_free_irq_vectors(g_pdev);
err_disable:   pci_disable_device(g_pdev);
err_put:       pci_dev_put(g_pdev);
    return ret;
}

static void __exit nvme_irq_mod_exit(void)
{
    misc_deregister(&nvme_irq_miscdev);
    free_irq(g_irq, NULL);
    pci_free_irq_vectors(g_pdev);
    if (g_efd) eventfd_ctx_put(g_efd);
    pci_disable_device(g_pdev);
    pci_dev_put(g_pdev);
    pr_info("nvme_irq: unloaded\n");
}

module_init(nvme_irq_mod_init);
module_exit(nvme_irq_mod_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NVMe MSI-X to eventfd bridge");
