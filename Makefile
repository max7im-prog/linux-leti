obj-m += simplefs.o

simplefs-objs := main.o super.o inode.o file.o ioctl.o disk.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

ccflags-y += -Wall -Wextra

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
