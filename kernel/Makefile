# Makefile — Build mycam.ko kernel module
#
# Gọi kernel build system (out-of-tree module build)
# Tự động sign module sau khi build (Secure Boot)
# Sử dụng: make [clean|sign|load|unload|reload]

KDIR    ?= /lib/modules/$(shell uname -r)/build
PWD     := $(shell pwd)
SIGN_KEY := $(PWD)/signing/MOK.priv
SIGN_CER := $(PWD)/signing/MOK.der

# Compiler flags: thêm debug info, tắt warning không cần thiết
ccflags-y := -Wall -Wno-unused-parameter

all: build sign

build:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Sign module cho Secure Boot — chạy tự động sau mỗi build
sign: mycam.ko
	@if [ -f "$(SIGN_KEY)" ] && [ -f "$(SIGN_CER)" ]; then \
		echo "[mycam] Signing mycam.ko for Secure Boot..."; \
		$(KDIR)/scripts/sign-file sha256 $(SIGN_KEY) $(SIGN_CER) mycam.ko; \
		echo "[mycam] Module signed OK"; \
	else \
		echo "[mycam] WARNING: signing keys not found, skipping sign"; \
	fi

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

.PHONY: all build sign clean load unload reload
