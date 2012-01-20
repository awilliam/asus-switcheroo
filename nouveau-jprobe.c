/*
 * Jprobes hack to disable the nouveau interrupt handler when device
 * is powered down.
 *
 * Copyright 2011 Red Hat, Inc
 *
 * Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/kprobes.h>
#include <linux/kallsyms.h>
#include <linux/pci.h>
#include <linux/workqueue.h>

static unsigned int nouveau_irq;
static irqreturn_t (*nouveau_irq_handler)(int irq, void *arg);
static unsigned long nouveau_flags;
static const char *nouveau_name;
static void *nouveau_dev;
static struct pci_dev *nouveau_pdev;
static int nouveau_irq_disabled;

static int my_nouveau_pci_suspend(struct pci_dev *pdev, pm_message_t pm_state);

static struct jprobe my_nouveau_pci_suspend_jprobe = {
	.entry = (kprobe_opcode_t *)my_nouveau_pci_suspend
};

static void unregister_pci_suspend(struct work_struct *work)
{
	unregister_jprobe(&my_nouveau_pci_suspend_jprobe);
	my_nouveau_pci_suspend_jprobe.kp.addr = NULL;
}

DECLARE_WORK(unregister_pci_suspend_work, unregister_pci_suspend);

/* This jprobe is simply to find the struct pci_dev for the nouveau card */
static int my_nouveau_pci_suspend(struct pci_dev *pdev, pm_message_t pm_state)
{
	if (!nouveau_pdev) {
		printk("Discovered nouveau pdev: %p\n", pdev);
		nouveau_pdev = pdev;
		schedule_work(&unregister_pci_suspend_work);
	}

 	jprobe_return();
	return 0; /* unreached */
}

/* Register the jprobe in a workqueue to get it out of interrupt context */
static void register_pci_suspend(struct work_struct *work)
{
	if (register_jprobe(&my_nouveau_pci_suspend_jprobe) < 0) {
		printk("Failed to register nouveau_pci_suspend jprobe\n");
		my_nouveau_pci_suspend_jprobe.kp.addr = NULL;
		return;
	}
	printk("nouveau_pci_suspend jprobe registered\n");
}

static DECLARE_WORK(register_pci_suspend_work, register_pci_suspend);

static int my_request_threaded_irq(unsigned int irq, irq_handler_t handler,
				   irq_handler_t thread_fn, unsigned long flags,
				   const char *name, void *dev);

static struct jprobe my_request_threaded_irq_jprobe = {
	.entry = (kprobe_opcode_t *)my_request_threaded_irq
};

static void unregister_request_threaded_irq(struct work_struct *work)
{
	unregister_jprobe(&my_request_threaded_irq_jprobe);
	my_request_threaded_irq_jprobe.kp.addr = NULL;
}

DECLARE_WORK(unregister_request_threaded_irq_work,
	     unregister_request_threaded_irq);

/* Intercept calls to request_threaded_irq().  Here we can check if nouveau
 * is registered yet, finding the irq handler and suspend function.  If this
 * calls is for nouveau, we record all the parameters so we can use them to
 * free the irq and re-request it later. */
static int my_request_threaded_irq(unsigned int irq, irq_handler_t handler,
				   irq_handler_t thread_fn, unsigned long flags,
				   const char *name, void *dev)
{
	if (!nouveau_irq_handler) {
		nouveau_irq_handler =
			(void *)kallsyms_lookup_name("nouveau_irq_handler");
		my_nouveau_pci_suspend_jprobe.kp.addr =
			(void *)kallsyms_lookup_name("nouveau_pci_suspend");
		if (my_nouveau_pci_suspend_jprobe.kp.addr)
			schedule_work(&register_pci_suspend_work);
		else {
			nouveau_irq_handler = NULL;
			printk("Failed to find nouveau_pci_suspend\n");
		}
	}

	if (nouveau_irq_handler && handler == nouveau_irq_handler) {
		printk("Discovered nouveau irq params\n");
		nouveau_irq = irq;
		nouveau_flags = flags;
		nouveau_name = name;
		nouveau_dev = dev;
		schedule_work(&unregister_request_threaded_irq_work);
	}

 	jprobe_return();
	return 0; /* unreached */
}

/* We can't call request_irq from interrupt context, so push this out to
 * a workqueue too. */
static void my_nouveau_reenable_irq_work(struct work_struct *work)
{
	int ret;

	if (!nouveau_irq_disabled)
		return;

	printk("Re-enabling nouveau irq handler\n");
	ret = request_irq(nouveau_irq, nouveau_irq_handler,
			  nouveau_flags, nouveau_name, nouveau_dev);
	if (ret < 0)
		printk("Failed to re-request nouveau irq: %d\n", ret);
	nouveau_irq_disabled = 0;
}

static DECLARE_WORK(my_nouveau_reenable_irq_register_work,
		    my_nouveau_reenable_irq_work);

/* Here's where we disable and re-enable the nouveau irq handler.  We disable
 * in this function if we're going to D3hot and we setup the kretprobe handler
 * to get called to re-enable if going back to D0.  This is the only probe
 * that remains enabled once the jprobes find all the data. */
static int my_pci_set_power_state_pre_kprobe(struct kretprobe_instance *ri,
					     struct pt_regs *regs)
{
	struct pci_dev *pdev;
	pci_power_t state;

#ifdef CONFIG_X86_64
	pdev = (struct pci_dev *)regs->di;
	state = (pci_power_t)regs->si;
#else
	pdev = (struct pci_dev *)regs->ax;
	state = (pci_power_t)regs->dx;
#endif

	if (nouveau_irq_handler && pdev == nouveau_pdev) {
		if (state == PCI_D0)
			return 0; /* call handler */
		else if (state == PCI_D3hot && !nouveau_irq_disabled) {
			printk("Disabling nouveau irq handler\n");
			free_irq(nouveau_irq, nouveau_dev);
			nouveau_irq_disabled = 1;
		}
	}
	return 1; /* don't call handler */
}

static int my_pci_set_power_state_kprobe(struct kretprobe_instance *ri,
					 struct pt_regs *regs)
{
	if (nouveau_irq_disabled)
		schedule_work(&my_nouveau_reenable_irq_register_work);
	return 0;
}

static struct kretprobe my_pci_set_power_state_kretprobe = {
	.entry_handler = (kretprobe_handler_t)my_pci_set_power_state_pre_kprobe,
	.handler = (kretprobe_handler_t)my_pci_set_power_state_kprobe,
	.maxactive = NR_CPUS,
};

int __init nouveau_jprobe_init(void)
{
	int ret;

	/* Register jprobe for request_threaded_irq, this is our hook to
         * find the parameters to call this ourselves and setup another
         * jprobe hook to find the pci device for nouveau. */
	my_request_threaded_irq_jprobe.kp.addr =
		(kprobe_opcode_t *)kallsyms_lookup_name("request_threaded_irq");
	if (!my_request_threaded_irq_jprobe.kp.addr) {
		printk("Couldn't find request_threaded_irq address\n");
		return -1;
	}

	if ((ret = register_jprobe(&my_request_threaded_irq_jprobe)) < 0) {
		printk("Failed register_jprobe for request_irq, %d\n", ret);
		return -1;
	}

	/* This is the long running hook that toggles the interrupt handler
         * so that it gets unregistered while the device is off. */
	my_pci_set_power_state_kretprobe.kp.addr =
		(kprobe_opcode_t *)kallsyms_lookup_name("pci_set_power_state");
	if (!my_pci_set_power_state_kretprobe.kp.addr) {
		printk("Couldn't find pci_set_power_state address\n");
		return -1;
	}
	if ((ret = register_kretprobe(&my_pci_set_power_state_kretprobe)) < 0) {
		printk("Failed register_kretprobe for pci_set_power_state, "
		       "%d\n", ret);
		return -1;
	}
		
	printk("Registered nouveau jprobe\n");

	return 0;
}

void __exit nouveau_jprobe_exit(void)
{
	unregister_kretprobe(&my_pci_set_power_state_kretprobe);

	if (my_request_threaded_irq_jprobe.kp.addr)
		unregister_jprobe(&my_request_threaded_irq_jprobe);
	if (my_nouveau_pci_suspend_jprobe.kp.addr)
		unregister_jprobe(&my_nouveau_pci_suspend_jprobe);

	printk("Unregistered nouveau jprobe\n");
}

module_init(nouveau_jprobe_init);
module_exit(nouveau_jprobe_exit);

MODULE_AUTHOR("Alex Williamson <alex.williamson@redhat.com>");
MODULE_DESCRIPTION("Jprobe hack to fix nouveau bugs when device is disabled");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.1");
