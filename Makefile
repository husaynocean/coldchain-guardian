# Coldchain Guardian
#
#   make qnx            # QNX 8.0, x86_64 target (VMware / VirtualBox / QEMU)
#   make qnx-arm        # QNX 8.0, aarch64le (Raspberry Pi 5)
#   make linux          # host build for UI development only (no RT guarantees)
#
# For the QNX builds, source the SDP environment first:
#   Linux host:   source ~/qnx800/qnxsdp-env.sh
#   Windows host: run qnxsdp-env.bat from the SDP install folder

SRC    := $(wildcard src/*.c)
CFLAGS := -O2 -Wall -Wextra -Wno-format-truncation -std=gnu11

.PHONY: all qnx qnx-arm linux clean

all: linux

qnx: build/x86_64/coldchaind
qnx-arm: build/aarch64le/coldchaind
linux: build/linux/coldchaind

build/x86_64/coldchaind: $(SRC) src/cc.h
	@mkdir -p $(dir $@)
	qcc -Vgcc_ntox86_64 $(CFLAGS) -o $@ $(SRC) -lsocket -lm

build/aarch64le/coldchaind: $(SRC) src/cc.h
	@mkdir -p $(dir $@)
	qcc -Vgcc_ntoaarch64le $(CFLAGS) -o $@ $(SRC) -lsocket -lm

build/linux/coldchaind: $(SRC) src/cc.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -pthread -o $@ $(SRC) -lm

clean:
	rm -rf build
