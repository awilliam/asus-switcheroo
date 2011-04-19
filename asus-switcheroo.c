/*
 * VGA switcheroo driver for ASUS laptops
 *
 * Based on drivers/gpu/drm/nouveau/nouveau_acpi.c
 *
 * Copyright 2011 Red Hat, Inc
 *
 * Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <linux/pci.h>
#include <linux/acpi.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/kallsyms.h>
#include <linux/vga_switcheroo.h>
#include <acpi/acpi_bus.h>
#include <acpi/acpi_drivers.h>
#include <acpi/video.h>

#define DSM_SUPPORTED 0x00
#define DSM_SUPPORTED_FUNCTIONS 0x00

#define DSM_LED 0x02
#define DSM_LED_STATE 0x00
#define DSM_LED_OFF 0x10
#define DSM_LED_STAMINA 0x11
#define DSM_LED_SPEED 0x12

#define DSM_POWER 0x03
#define DSM_POWER_STATE 0x00
#define DSM_POWER_SPEED 0x01
#define DSM_POWER_STAMINA 0x02

static acpi_handle dsm_handle;
static acpi_handle discrete_handle;
static acpi_handle igd_handle;

static struct pci_dev *discrete_dev;
static bool dummy_client;
static bool dummy_client_switched;

static const char dsm_uuid[] = {
	0xA0, 0xA0, 0x95, 0x9D, 0x60, 0x00, 0x48, 0x4D,
	0xB3, 0x4D, 0x7E, 0x5F, 0xEA, 0x12, 0x9F, 0xD4,
};

static int asus_switcheroo_dsm_call(acpi_handle handle, int func, int arg)
{
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_object_list input;
	union acpi_object params[4], elements[4];
	union acpi_object *obj;
	int i, err;

	input.count = 4;
	input.pointer = params;
	params[0].type = ACPI_TYPE_BUFFER;
	params[0].buffer.length = sizeof(dsm_uuid);
	params[0].buffer.pointer = (char *)dsm_uuid;
	params[1].type = ACPI_TYPE_INTEGER;
	params[1].integer.value = 0; /* revision ID - unused */
	params[2].type = ACPI_TYPE_INTEGER;
	params[2].integer.value = func;
	params[3].type = ACPI_TYPE_PACKAGE;
	params[3].package.count = 4;
	params[3].package.elements = elements;
	for (i = 0; i < 4; i++) {
		elements[i].type = ACPI_TYPE_INTEGER;
		elements[i].integer.value = (arg >> (i * 8)) & 0xff;
	}

	err = acpi_evaluate_object(handle, "_DSM", &input, &output);
	if (err) {
		printk(KERN_INFO "failed to evaluate _DSM: %d\n", err);
		return err;
	}

	obj = (union acpi_object *)output.pointer;

	if (obj->type == ACPI_TYPE_INTEGER)
		if (obj->integer.value == 0x80000002)
			return -ENODEV;

	kfree(output.pointer);
	return 0;
}

static int asus_switcheroo_acpi_mux(acpi_handle handle)
{
	struct acpi_object_list input;
	union acpi_object param;
	int err;

	input.count = 1;
	input.pointer = &param;
	param.type = ACPI_TYPE_INTEGER;
	param.integer.value = 1;

	/* I don't really know what these do, but it seems to work */
	err = acpi_evaluate_object(handle, "MXMX", &input, NULL);
	if (err) {
		printk(KERN_INFO "failed to evaluate MXMX: %d\n", err);
		return err;
	}

	err = acpi_evaluate_object(handle, "MXDS", &input, NULL);
	if (err) {
		printk(KERN_INFO "failed to evaluate MXMX: %d\n", err);
		return err;
	}
	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,37)
static void asus_switcheroo_force_nouveau_reprobe(void)
{
	void *dev = pci_get_drvdata(discrete_dev);
	void (*nouveau_fbcon_hook)(void *);

	nouveau_fbcon_hook =
		(void *)kallsyms_lookup_name("nouveau_fbcon_output_poll_changed");

	if (!nouveau_fbcon_hook) {
		printk("Can't hook to nouveau_fbcon_output_poll_changed\n");
		return;
	}
	nouveau_fbcon_hook(dev);
}
#endif

static int asus_switcheroo_switchto(enum vga_switcheroo_client_id id)
{
	int ret, dsm_arg;

	if (id == VGA_SWITCHEROO_IGD) {
		asus_switcheroo_acpi_mux(igd_handle);
		dsm_arg = DSM_LED_STAMINA;
	} else {
		asus_switcheroo_acpi_mux(discrete_handle);
		dsm_arg = DSM_LED_SPEED;
	}

	ret = asus_switcheroo_dsm_call(dsm_handle, DSM_LED, dsm_arg);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,37)
	if (id == VGA_SWITCHEROO_DIS && !dummy_client)
		asus_switcheroo_force_nouveau_reprobe();
#endif
	return ret;
}

static int asus_switcheroo_power_state(enum vga_switcheroo_client_id id,
				     enum vga_switcheroo_state state)
{
	int dsm_arg;

	if (id == VGA_SWITCHEROO_IGD)
		return 0;

	if (state == VGA_SWITCHEROO_ON)
		dsm_arg = DSM_POWER_SPEED;
	else
		dsm_arg = DSM_POWER_STAMINA;

	return asus_switcheroo_dsm_call(dsm_handle, DSM_POWER, dsm_arg);
}

static int asus_switcheroo_handler_init(void)
{
	return 0;
}

static int asus_switcheroo_get_client_id(struct pci_dev *pdev)
{
	if (pdev->vendor == PCI_VENDOR_ID_INTEL)
		return VGA_SWITCHEROO_IGD;

	return VGA_SWITCHEROO_DIS;
}

static struct vga_switcheroo_handler asus_dsm_handler = {
	.switchto = asus_switcheroo_switchto,
	.power_state = asus_switcheroo_power_state,
	.init = asus_switcheroo_handler_init,
	.get_client_id = asus_switcheroo_get_client_id,
};

static void asus_switcheroo_set_state(struct pci_dev *pdev,
				      enum vga_switcheroo_state state)
{
	if (state == VGA_SWITCHEROO_ON) {
		printk(KERN_INFO
		       "Asus switcheroo: turning on discrete graphics\n");
		pci_set_power_state(pdev, PCI_D0);
		pci_restore_state(pdev);
		if (pci_enable_device(pdev))
			printk(KERN_WARNING
			       "Asus switcher: failed to enable %s\n",
			       dev_name(&pdev->dev));
		pci_set_master(pdev);
		dummy_client_switched = true;
	} else {
		printk(KERN_INFO
		       "Asus switcheroo: turning off discrete graphics\n");
		pci_save_state(pdev);
		pci_clear_master(pdev);
		pci_disable_device(pdev);
		pci_set_power_state(pdev, PCI_D3hot);
	}
}

static bool asus_switcheroo_can_switch(struct pci_dev *pdev)
{
	return !dummy_client_switched;
}

static bool asus_switcheroo_dsm_pci_probe(struct pci_dev *pdev)
{
	acpi_handle handle, test_handle;
	acpi_status status;
	int ret;

	handle = DEVICE_ACPI_HANDLE(&pdev->dev);
	if (!handle)
		return false;

	status = acpi_get_handle(handle, "_DSM", &test_handle);
	if (ACPI_FAILURE(status)) {
		return false;
	}

	ret = asus_switcheroo_dsm_call(handle, DSM_SUPPORTED,
				       DSM_SUPPORTED_FUNCTIONS);
	if (ret < 0)
		return false;

	status = acpi_get_handle(handle, "MXMX", &test_handle);
	if (ACPI_FAILURE(status)) {
		return false;
	}

	status = acpi_get_handle(handle, "MXDS", &test_handle);
	if (ACPI_FAILURE(status)) {
		return false;
	}

	dsm_handle = handle;
	return true;
}

static bool asus_switcheroo_dsm_detect(void)
{
	struct pci_dev *pdev = NULL;
	int class = PCI_CLASS_DISPLAY_VGA << 8;
	int vga_count = 0;

	while ((pdev = pci_get_class(class, pdev)) != NULL) {
		struct acpi_buffer buf = { ACPI_ALLOCATE_BUFFER, NULL };
		acpi_handle handle;

		vga_count++;

		/* The _DSM actually exists on both devices on this system */
		if (!asus_switcheroo_dsm_pci_probe(pdev))
			return false;

		handle = DEVICE_ACPI_HANDLE(&pdev->dev);
		if (!handle)
			return false;

		if (pdev->vendor == PCI_VENDOR_ID_INTEL) {
			igd_handle = handle;
		} else {
			discrete_handle = handle;
			discrete_dev = pdev;
		}

		acpi_get_name(handle, ACPI_FULL_PATHNAME, &buf);
		printk(KERN_INFO "Found VGA device %s (%s): %s\n",
		       dev_name(&pdev->dev), (char *)buf.pointer,
		       pdev->vendor == PCI_VENDOR_ID_INTEL ? "IGD" : "DIS");
		kfree(buf.pointer);
	}

	if (vga_count == 2 && dsm_handle && igd_handle && discrete_handle) {
		struct acpi_buffer buf = { ACPI_ALLOCATE_BUFFER, NULL };

		acpi_get_name(dsm_handle, ACPI_FULL_PATHNAME, &buf);
		printk(KERN_INFO
		       "Asus switcheroo: detected DSM switching method "
		       "%s handle\n", (char *)buf.pointer);
		kfree(buf.pointer);
		return true;
	}
	return false;
}

static int __init asus_switcheroo_init(void)
{
	if (!asus_switcheroo_dsm_detect())
		return 0;

	vga_switcheroo_register_handler(&asus_dsm_handler);

	if (dummy_client)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,37)
		vga_switcheroo_register_client(discrete_dev,
					       asus_switcheroo_set_state, NULL,
					       asus_switcheroo_can_switch);
#else
		vga_switcheroo_register_client(discrete_dev,
					       asus_switcheroo_set_state,
					       asus_switcheroo_can_switch);
#endif
	return 0;
}

static void __exit asus_switcheroo_exit(void)
{
	if (dummy_client)
		vga_switcheroo_unregister_client(discrete_dev);
	vga_switcheroo_unregister_handler();
}

module_init(asus_switcheroo_init);
module_exit(asus_switcheroo_exit);

module_param(dummy_client, bool, 0444);
MODULE_PARM_DESC(dummy_client, "Enable dummy VGA switcheroo client support");

MODULE_AUTHOR("Alex Williamson <alex.williamson@redhat.com>");
MODULE_DESCRIPTION("Experimental Asus hybrid graphics switcheroo");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.1");
