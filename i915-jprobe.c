/*
 * Jprobes hack to disable the i915 lid notifier when the device
 * is powered down.
 *
 * Copyright 2011 Red Hat, Inc
 *
 * Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/kallsyms.h>
#include <linux/notifier.h>
#include <linux/vga_switcheroo.h>
#include <linux/workqueue.h>

struct notifier_block *i915_lid_nb;
int (*i915_lid_notify)(struct notifier_block *, unsigned long , void *);

static int my_dummy_lid_notify(struct notifier_block *nb, unsigned long val,
			       void *unused)
{
	printk("Dummy i915 lid notify called\n");
	return NOTIFY_OK;
}

static void my_i915_switcheroo_set_state(struct pci_dev *pdev,
					 enum vga_switcheroo_state state)
{
	if (!i915_lid_nb) {
		printk("Switching state, but no notifier block found\n");
		goto done;
	}

	if (state == VGA_SWITCHEROO_ON) {
		printk("Re-enabling i915 lid notifier\n");
		i915_lid_nb->notifier_call = i915_lid_notify;
	} else {
		printk("Disabling i915 lid notifier\n");
		i915_lid_nb->notifier_call = my_dummy_lid_notify;
	}

done:
 	jprobe_return();
	return; /* unreached */
}

static struct jprobe my_i915_switcheroo_set_state_jprobe = {
	.entry = (kprobe_opcode_t *)my_i915_switcheroo_set_state
};

static void i915_register_jprobe(struct work_struct *work)
{
	if (register_jprobe(&my_i915_switcheroo_set_state_jprobe) < 0) {
		printk("Failed to register i915 jprobe\n");
		i915_lid_notify = NULL;
		return;
	}
	printk("i915 jprobe registered\n");
}

static DECLARE_WORK(i915_jprobe_register_work, i915_register_jprobe);

static int my_acpi_lid_notifier_register(struct notifier_block *nb)
{
	if (!i915_lid_notify) {
		i915_lid_notify = (void *)kallsyms_lookup_name("intel_lid_notify");
		if (!i915_lid_notify)
			goto done;

		my_i915_switcheroo_set_state_jprobe.kp.addr =
			(kprobe_opcode_t *)kallsyms_lookup_name("i915_switcheroo_set_state");
		if (!my_i915_switcheroo_set_state_jprobe.kp.addr) {
			i915_lid_notify = NULL;
			goto done;
		}
		schedule_work(&i915_jprobe_register_work);
	}

	if (nb->notifier_call == i915_lid_notify) {
		printk("Matched i915 lid notifier block %p\n", nb);
		i915_lid_nb = nb;
	}

done:
 	jprobe_return();
	return 0; /* unreached */
}

static struct jprobe my_acpi_lid_notifier_register_jprobe = {
	.entry = (kprobe_opcode_t *)my_acpi_lid_notifier_register
};

int init_module(void)
{
	int ret;

	my_acpi_lid_notifier_register_jprobe.kp.addr =
		(kprobe_opcode_t *)kallsyms_lookup_name("acpi_lid_notifier_register");
	if (!my_acpi_lid_notifier_register_jprobe.kp.addr) {
		printk("Couldn't find acpi_lid_notifier_register address\n");
		return -1;
	}

	if ((ret = register_jprobe(&my_acpi_lid_notifier_register_jprobe)) < 0) {
		printk("Failed register_jprobe for acpi_lid_notifier_register, %d\n",
		       ret);
		return -1;
	}
	printk("Registered i915/lid jprobe\n");

	return 0;
}

void cleanup_module(void)
{
	unregister_jprobe(&my_acpi_lid_notifier_register_jprobe);
	if (my_i915_switcheroo_set_state_jprobe.kp.addr)
		unregister_jprobe(&my_i915_switcheroo_set_state_jprobe);
	printk("Unregistered i915/lid jprobe\n");
}

MODULE_AUTHOR("Alex Williamson <alex.williamson@redhat.com>");
MODULE_DESCRIPTION("Jprobe hack to fix i915 bugs when device is disabled");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.1");
