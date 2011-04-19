obj-m := asus-switcheroo.o i915-jprobe.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install-fedora:
	install -m 0644 -D asus-switcheroo.ko /lib/modules/$(shell uname -r)/extra/asus-switcheroo/asus-switcheroo.ko
	depmod -a
	install -m 0755 asus-switcheroo-pm /etc/pm/sleep.d/75-asus-switcheroo-pm
	install -m 0644 asus-switcheroo.conf-modprobe.d /etc/modprobe.d/asus-switcheroo.conf
	install -m 0644 asus-switcheroo.conf-dracut /etc/dracut.conf.d/asus-switcheroo.conf
	cp /boot/initramfs-$(shell uname -r).img /boot/initramfs-$(shell uname -r).img.bak
	dracut -f /boot/initramfs-$(shell uname -r).img $(shell uname -r)

uninstall-fedora:
	rm -fr /lib/modules/$(shell uname -r)/extra/asus-swticheroo
	depmod -a
	rm -f /etc/pm/sleep.d/75-asus-switcheroo-pm
	rm -f /etc/modprobe.d/asus-switcheroo.conf
	rm -f /etc/dracut.conf.d/asus-switcheroo.conf
	dracut -f /boot/initramfs-$(shell uname -r).img $(shell uname -r)
	
install-ubuntu:
	install -m 0644 -D asus-switcheroo.ko /lib/modules/$(shell uname -r)/extra/asus-switcheroo/asus-switcheroo.ko
	depmod -a
	install -m 0755 asus-switcheroo-pm /etc/pm/sleep.d/75-asus-switcheroo-pm
	install -m 0644 asus-switcheroo.conf-modprobe.d /etc/modprobe.d/asus-switcheroo.conf
	sed -i -e "/asus-switcheroo/D" /etc/initramfs-tools/modules
	echo asus-switcheroo >> /etc/initramfs-tools/modules
	cp /boot/initrd.img-$(shell uname -r) /boot/initrd.img-$(shell uname -r).bak
	update-initramfs -u -k $(shell uname -r)

uninstall-ubuntu:
	rm -fr /lib/modules/$(shell uname -r)/extra/asus-switcheroo
	depmod -a
	rm -f /etc/pm/sleep.d/75-asus-switcheroo-pm
	rm -f /etc/modprobe.d/asus-switcheroo.conf
	sed -i -e "/asus-switcheroo/D" /etc/initramfs-tools/modules
	update-initramfs -u -k $(shell uname -r)

install-arch:
	install -m 0644 -D asus-switcheroo.ko /lib/modules/$(uname -r)/extra/asus-switcheroo/asus-switcheroo.ko
	depmod -a
	install -m 0755 asus-switcheroo-pm /etc/pm/sleep.d/75-asus-switcheroo-pm
	install -m 0644 asus-switcheroo.conf-modprobe.d /etc/modprobe.d/asus-switcheroo.conf
	sed -i -e "s/MODULES=(/MODULES=(asus-switcheroo /" /etc/rc.conf
	cp /boot/kernel26.img /boot/kernel26.img.bak
	mkinitcpio -p kernel26

uninstall-arch:
	rm -fr /lib/modules/$(uname -r)/extra/asus-switcheroo
	depmod -a
	rm -f /etc/pm/sleep.d/75-asus-switcheroo-pm
	rm -f /etc/modprobe.d/asus-switcheroo.conf
	sed -i -e "s/MODULES=(asus-switcheroo /MODULES=(/" /etc/rc.conf
	mkinitcpio -p kernel26
