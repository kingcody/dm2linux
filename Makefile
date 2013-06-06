
obj-m	:= dm2.o

KDIR	:= /lib/modules/$(shell uname -r)/build
PWD	:= $(shell pwd)

IDIR	:= $(DESTDIR)/lib/modules/$(shell uname -r)/kernel/sound/drivers

module: default

default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

install: default
	mkdir -p $(IDIR)
	install dm2.ko $(IDIR)
	depmod -a

uninstall:
	rm $(IDIR)/dm2.ko

dist:
	ln -s . dm2
	tar cvjf dm2.tar.bz2  dm2/{dm2.c,dm2.h,DM2.midi.xml,LICENSE.txt,linux-lowspeedbulk.patch,Makefile,README}
	rm dm2

clean:
	rm -rf .*.cmd *.o *.ko .tmp* Module.symvers *.mod.c
