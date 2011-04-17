obj-m := asus-switcheroo.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	install -m 0644 -D asus-switcheroo.ko /lib/modules/$(shell uname -r)/extra/asus-switcheroo/asus-switcheroo.ko
	install -m 0755 asus-switcheroo-pm /etc/pm/sleep.d/75-asus-switcheroo-pm
	install -m 0644 asus-switcheroo.conf-modprobe.d /etc/modprobe.d/asus-switcheroo.conf
	install -m 0644 asus-switcheroo.conf-dracut /etc/dracut.conf.d/asus-switcheroo.conf
	depmod -a
	cp /boot/initramfs-$(shell uname -r).img /boot/initramfs-$(shell uname -r).img.bak
	dracut -f /boot/initramfs-$(shell uname -r).img $(shell uname -r)
