# Если KERNELRELEASE определен, значит вызов сделан из
# системы сборки ядра и можно использовать ее язык
ifneq ($(KERNELRELEASE),)
   obj-m := sbull.o
# Иначе вызов сделан прямо из командной
# строки; вызвать систему сборки ядра.
else
   KERNELDIR ?= /lib/modules/$(shell uname -r)/build
   PWD := $(shell pwd)
default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
endif
