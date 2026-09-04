# raspbootin64 stub — cross-built with the same toolchain as the main project.
# Produces raspbootin64.img: copy it to the SD card boot partition AS kernel8.img.

CROSS   ?= aarch64-linux-gnu-
CC      = $(CROSS)gcc
LD      = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

CFLAGS  = -Wall -O2 -ffreestanding -nostdinc -nostdlib -nostartfiles -mcpu=cortex-a53

OBJS = start.o main.o uart.o mbox.o

all: raspbootin64.img

start.o: start.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

raspbootin64.img: $(OBJS) link.ld
	$(LD) $(OBJS) -T link.ld -o raspbootin64.elf
	$(OBJCOPY) -O binary raspbootin64.elf raspbootin64.img
	@sz=$$(stat -c %s raspbootin64.img); echo "raspbootin64.img = $$sz bytes"; \
	 if [ $$sz -gt 1024 ]; then echo "WARNING: loader > 1024 B, will overlap load address"; exit 1; fi

clean:
	rm -f *.o *.elf *.img
