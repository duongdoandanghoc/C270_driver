# Makefile — Build mycam.ko kernel module
#
# Gọi kernel build system (out-of-tree module build)
# Sử dụng: make [clean]

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# Compiler flags: thêm debug info, tắt warning không cần thiết
ccflags-y := -Wall -Wno-unused-parameter

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Load/unload helpers (cần sudo)
load: all
	sudo rmmod mycam 2>/dev/null || true
	sudo insmod mycam.ko
	@echo "[mycam] Module loaded. Check: dmesg | tail -20"

unload:
	sudo rmmod mycam
	@echo "[mycam] Module unloaded"

reload: unload load

.PHONY: all clean load unload reload
