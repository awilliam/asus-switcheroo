/*
 * Build-Your-Own VGA switcheroo driver
 *
 * Copyright 2011 Red Hat, Inc
 *
 * Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/acpi.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/kallsyms.h>
#include <linux/vga_switcheroo.h>
#include <acpi/acpi_bus.h>
#include <acpi/acpi_drivers.h>
#include <acpi/video.h>

static int igd_vendor = PCI_VENDOR_ID_INTEL;
static char *switchto_igd = NULL;
static char *switchto_dis = NULL;
static char *power_state_igd_on = NULL;
static char *power_state_igd_off = NULL;
static char *power_state_dis_on = NULL;
static char *power_state_dis_off = NULL;
static bool dummy_client;
static bool dummy_client_switched;

static struct pci_dev *igd_dev, *dis_dev;
static acpi_handle igd_handle, dis_handle;

static acpi_status do_acpi_call(const char *method, int argc, union acpi_object *argv)
{
	acpi_status status;
	acpi_handle handle;
	struct acpi_object_list arg;

	/* get the handle of the method, must be a fully qualified path */
	status = acpi_get_handle(NULL, (acpi_string)method, &handle);

	if (ACPI_FAILURE(status)) {
		printk(KERN_ERR "acpi_call: Cannot get handle: %s\n", acpi_format_exception(status));
		return status;
	}

	/* prepare parameters */
	arg.count = argc;
	arg.pointer = argv;

	/* call the method */
	status = acpi_evaluate_object(handle, NULL, &arg, NULL);
	if (ACPI_FAILURE(status)) {
		printk(KERN_ERR "acpi_call: Method call failed: %s\n", acpi_format_exception(status));
		return status;
	}
	return 0;
}

static u8 decodeHex(char *hex) {
	char buf[3] = { hex[0], hex[1], 0 };
	return (u8)simple_strtoul(buf, NULL, 16);
}

static char *parse_acpi_args(char *input, int *nargs, union acpi_object **args)
{
	char *s = input;

	*nargs = 0;
	*args = NULL;

	/* the method name is separated from the arguments by a space */
	while (*s && *s != ' ')
		s++;
	/* if no space is found, return 0 arguments */
	if (*s == 0)
		return input;
    
	*args = (union acpi_object *)kmalloc(16 * sizeof(union acpi_object), GFP_KERNEL);

	while (*s) {
		if (*s == ' ') {
			if (*nargs == 0)
				*s = 0; /* change first space to nul */
			(*nargs)++;
			s++;
		} else {
			union acpi_object *arg = (*args) + (*nargs - 1);
			if (*s == '"') {
				/* decode string */
				arg->type = ACPI_TYPE_STRING;
				arg->string.pointer = ++s;
				arg->string.length = 0;
				while (*s && *s++ != '"')
					arg->string.length++;
				/* skip the last " */
				s++;
			} else if (*s == 'b') {
				/* decode buffer - bXXXX */
				char *p = ++s;
				int len = 0, i;
				u8 *buf = NULL;
            
				while (*p && *p != ' ')
					p++;
                
				len = p - s;
				if (len % 2 == 1) {
					printk(KERN_ERR "acpi_call: buffer arg%d is not multiple of 8 bits\n", *nargs);
					return NULL;
				}
				len /= 2;

				buf = (u8*)kmalloc(len, GFP_KERNEL);
				for (i = 0; i < len; i++)
					buf[i] = decodeHex(s + (i * 2));
				s = p;

				arg->type = ACPI_TYPE_BUFFER;
				arg->buffer.pointer = buf;
				arg->buffer.length = len;
			} else if (*s == '{') {
				/* decode buffer - { b1, b2 ...} */
				u8 *buf, tmp[256];
				arg->type = ACPI_TYPE_BUFFER;
				arg->buffer.length = 0;

				buf = tmp;

				while (*s && *s++ != '}') {
					if (buf >= tmp + sizeof(tmp))
						printk(KERN_ERR "buffer full\n");
					else if (*s >= '0' && *s <= '9') {
						/* decode integer into buffer */
						arg->buffer.length++;
						if (s[0] == '0' && s[1] == 'x')
							*buf++ = simple_strtol(s + 2, 0, 16);
						else
							*buf++ = simple_strtol(s, 0, 10);
					}
					/* skip until space or comma or '}' */
					while (*s && *s != ' ' && *s != ',' && *s != '}')
						s++;
				}
				/* store the result in new allocated buffer */
				buf = (u8*)kmalloc(arg->buffer.length, GFP_KERNEL);
				memcpy(buf, tmp, arg->buffer.length);
				arg->buffer.pointer = buf;
			} else {
				/* decode integer, N or 0xN */
				arg->type = ACPI_TYPE_INTEGER;
				if (s[0] == '0' && s[1] == 'x')
					arg->integer.value = simple_strtol(s + 2, 0, 16);
				else
					arg->integer.value = simple_strtol(s, 0, 10);
				while (*s && *s != ' ')
					s++;
			}
		}
	}

	return input;
}

static void run_special(char *cmd)
{
	if (!strcmp(cmd, "nouveau_fbcon_output_poll_changed")) {
		void *dev = pci_get_drvdata(dis_dev);
		void (*func)(void *);

		func = (void *)kallsyms_lookup_name(cmd);

		if (!func) {
			printk("Can't hook to %s\n", cmd);
			return;
		}
		func(dev);
	}
}

static int acpi_call(char *script, acpi_handle handle)
{
	int i, j;

	if (!script)
		return -EINVAL;

	for (i = 0, j = 0; j <= strlen(script); j++) {
		char buf[1024];
		int k, nargs;
    		union acpi_object *args;
    		char *method;
		acpi_status status;

		if (i == j && script[j] == ' ')
			i = ++j;

		if (i == j || (script[j] != 0 && script[j] != ';' && script[j] != '\n'))
			continue;

		memset(buf, 0, sizeof(buf));
		if (script[i] != '\\' && script[i] != '!' && handle) {
			struct acpi_buffer tmp = { ACPI_ALLOCATE_BUFFER, NULL };
			acpi_get_name(handle, ACPI_FULL_PATHNAME, &tmp);
			memcpy(buf, tmp.pointer, strlen(tmp.pointer));
			buf[strlen(buf)] = '.';
			kfree(tmp.pointer);
		}
		memcpy(buf + strlen(buf), &script[i], j - i);
		if (buf[strlen(buf)] == '\n')
			buf[strlen(buf)] = 0;

		if (buf[0] == '!')
			run_special(buf + 1);
		else {
			method = parse_acpi_args(buf, &nargs, &args);
			if (method) {
				status = do_acpi_call(method, nargs, args);
				if (args) {
					for (k = 0; k < nargs; k++)
						if (args[k].type == ACPI_TYPE_BUFFER)
							kfree(args[k].buffer.pointer);
					kfree(args);
				}
				if (ACPI_FAILURE(status))
					return status;
			} else
				return -EINVAL;
		}
		i = j + 1;
	}
	return 0;
}

static int byo_switcheroo_switchto(enum vga_switcheroo_client_id id)
{
	int ret;

	if (id == VGA_SWITCHEROO_IGD) {
		ret = acpi_call(switchto_igd, igd_handle);
	} else {
		ret = acpi_call(switchto_dis, dis_handle);
	}

	return ret;
}

static int byo_switcheroo_power_state(enum vga_switcheroo_client_id id,
				      enum vga_switcheroo_state state)
{
	int ret;

	if (id == VGA_SWITCHEROO_IGD) {
		if (state == VGA_SWITCHEROO_ON)
			ret = acpi_call(power_state_igd_on, igd_handle);
		else
			ret = acpi_call(power_state_igd_off, igd_handle);
	} else {
		if (state == VGA_SWITCHEROO_ON)
			ret = acpi_call(power_state_dis_on, dis_handle);
		else
			ret = acpi_call(power_state_dis_off, dis_handle);
	}

	return ret;
}

static int byo_switcheroo_handler_init(void)
{
	return 0;
}

static int byo_switcheroo_get_client_id(struct pci_dev *pdev)
{
	if (pdev->vendor == igd_vendor)
		return VGA_SWITCHEROO_IGD;

	return VGA_SWITCHEROO_DIS;
}

static struct vga_switcheroo_handler byo_switcheroo_handler = {
	.switchto = byo_switcheroo_switchto,
	.power_state = byo_switcheroo_power_state,
	.init = byo_switcheroo_handler_init,
	.get_client_id = byo_switcheroo_get_client_id,
};

static void dummy_switcheroo_set_state(struct pci_dev *pdev,
				       enum vga_switcheroo_state state)
{
	if (state == VGA_SWITCHEROO_ON) {
		printk(KERN_INFO
		       "BYO switcheroo: turning on discrete graphics\n");
		pci_set_power_state(pdev, PCI_D0);
		pci_restore_state(pdev);
		if (pci_enable_device(pdev))
			printk(KERN_WARNING
			       "BYO switcheroo: failed to enable %s\n",
			       dev_name(&pdev->dev));
		pci_set_master(pdev);
		dummy_client_switched = true;
	} else {
		printk(KERN_INFO
		       "BYO switcheroo: turning off discrete graphics\n");
		pci_save_state(pdev);
		pci_clear_master(pdev);
		pci_disable_device(pdev);
		pci_set_power_state(pdev, PCI_D3hot);
	}
}

static bool dummy_switcheroo_can_switch(struct pci_dev *pdev)
{
	return !dummy_client_switched;
}

static int __init byo_switcheroo_init(void)
{
	struct pci_dev *pdev = NULL;
	int ret, class = PCI_CLASS_DISPLAY_VGA << 8;

	while ((pdev = pci_get_class(class, pdev)) != NULL) {
		struct acpi_buffer buf = { ACPI_ALLOCATE_BUFFER, NULL };
		acpi_handle handle;

		handle = DEVICE_ACPI_HANDLE(&pdev->dev);
		if (!handle)
			continue;

		if (pdev->vendor == igd_vendor) {
			igd_dev = pdev;
			igd_handle = handle;
		} else {
			dis_dev = pdev;
			dis_handle = handle;
		}

		acpi_get_name(handle, ACPI_FULL_PATHNAME, &buf);
		printk(KERN_INFO "Found VGA device %s (%s): %s\n",
		       dev_name(&pdev->dev), (char *)buf.pointer,
		       pdev->vendor == igd_vendor ? "IGD" : "DIS");
		kfree(buf.pointer);
	}

	ret = vga_switcheroo_register_handler(&byo_switcheroo_handler);
	if (ret) {
		printk(KERN_ERR "BYO-switcheroo failed to register handler\n");
		return ret;
	}

	printk(KERN_INFO "BYO-switcheroo handler registered\n");

	if (dummy_client) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,38)
		ret = vga_switcheroo_register_client(dis_dev,
						     dummy_switcheroo_set_state, NULL,
						     dummy_switcheroo_can_switch);
#else
		ret = vga_switcheroo_register_client(dis_dev,
						     dummy_switcheroo_set_state,
						     dummy_switcheroo_can_switch);
#endif
		if (ret)
			printk(KERN_ERR "BYO-switcheroo failed to register dummy client\n");
		else
			printk(KERN_INFO "BYO-switcheroo dummy client registered\n");
	}
	return 0;
}

static void __exit byo_switcheroo_exit(void)
{
	if (dummy_client)
		vga_switcheroo_unregister_client(dis_dev);
	vga_switcheroo_unregister_handler();
}

module_init(byo_switcheroo_init);
module_exit(byo_switcheroo_exit);

module_param(dummy_client, bool, 0444);
MODULE_PARM_DESC(dummy_client, "Enable dummy VGA switcheroo client support");

module_param(igd_vendor, int, 0444);

module_param(switchto_igd, charp, 0644);
module_param(switchto_dis, charp, 0644);

module_param(power_state_igd_on, charp, 0644);
module_param(power_state_igd_off, charp, 0644);

module_param(power_state_dis_on, charp, 0644);
module_param(power_state_dis_off, charp, 0644);

MODULE_AUTHOR("Alex Williamson <alex.williamson@redhat.com>");
MODULE_DESCRIPTION("Build-Your-Own hybrid graphics switcheroo");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.1");
