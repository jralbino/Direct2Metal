# File: Makefile
TOOLCHAIN = aarch64-linux-gnu
CC = $(TOOLCHAIN)-gcc
CXX = $(TOOLCHAIN)-g++
LD = $(TOOLCHAIN)-ld
OBJCOPY = $(TOOLCHAIN)-objcopy
CXXFLAGS = -O3 -Wall -nostdlib -nostartfiles -ffreestanding -fno-exceptions -fno-rtti

all: kernel8.img

kernel8.img: start.o matmul_neon.o kernel.o
	$(LD) -T src/linker.ld -o kernel8.elf start.o matmul_neon.o kernel.o
	$(OBJCOPY) -O binary kernel8.elf kernel8.img

start.o: src/start.s
	$(CC) -c src/start.s -o start.o

matmul_neon.o: src/matmul_neon.s
	$(CC) -c src/matmul_neon.s -o matmul_neon.o

kernel.o: src/kernel.cpp
	$(CXX) $(CXXFLAGS) -c src/kernel.cpp -o kernel.o

run: kernel8.img
	qemu-system-aarch64 -M raspi3b -nographic -kernel kernel8.img

clean:
	rm -f *.o *.elf *.img