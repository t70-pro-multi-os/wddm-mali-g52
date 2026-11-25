# malikm on the host, against a mock WDK and a simulated G52. `make run`.
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-multichar \
          -D_KERNEL_MODE -Imock -I../../include -pthread
DRV = ../../drivers/malikm

COMMON = ../../drivers/common

SRC = test.c sim.c mock.c $(DRV)/gpu.c $(DRV)/uapi.c $(COMMON)/maligpu.c
DEPS = $(SRC) $(DRV)/malikm.h $(COMMON)/maligpu.h mock/ntddk.h sim.h

# The RK3576's G52 and the MT6789's G57 (the mt6789 branch's own build).
malikm-host: $(DEPS)
	$(CC) $(CFLAGS) -DMALI_SOC_RK3576 -o $@ $(SRC)

malikm-host-g57: $(DEPS)
	$(CC) $(CFLAGS) -DMALI_SOC_MT6789 -DSIM_GPU_ID=0x90930010u -DSIM_PROD=0x9093 -DSIM_REV=0x0010 -o $@ $(SRC)

run: malikm-host malikm-host-g57
	./malikm-host
	./malikm-host-g57

clean:
	rm -f malikm-host malikm-host-g57
