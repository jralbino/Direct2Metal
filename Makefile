# File: Makefile
TOOLCHAIN = aarch64-linux-gnu
CC = $(TOOLCHAIN)-gcc
CXX = $(TOOLCHAIN)-g++
LD = $(TOOLCHAIN)-ld
OBJCOPY = $(TOOLCHAIN)-objcopy

# Flags: -Isrc para encontrar los headers .h
CXXFLAGS = -O3 -Wall -nostdlib -nostartfiles -ffreestanding -fno-exceptions -fno-rtti -Isrc

all: kernel8.img

# --- LINKING ---
# AQUI AGREGAMOS conv2d_neon.o a la lista
kernel8.img: start.o matmul_neon.o data.o conv2d_neon.o kernel.o
	$(LD) -T src/linker.ld -o kernel8.elf start.o matmul_neon.o data.o conv2d_neon.o kernel.o
	$(OBJCOPY) -O binary kernel8.elf kernel8.img

# --- COMPILATION RULES ---

start.o: src/start.s
	$(CC) -c src/start.s -o start.o

# Regla para el viejo benchmark (opcional, pero lo dejamos)
matmul_neon.o: src/matmul_neon.s
	$(CC) -c src/matmul_neon.s -o matmul_neon.o

# Regla para los datos binarios (pesos)
data.o: src/data.s
	$(CC) -c src/data.s -o data.o

# Regla para la NUEVA Convolucion NEON
conv2d_neon.o: src/conv2d_neon.s
	$(CC) -c src/conv2d_neon.s -o conv2d_neon.o

kernel.o: src/kernel.cpp
	$(CXX) $(CXXFLAGS) -c src/kernel.cpp -o kernel.o

# --- RUN COMMAND ---
run: kernel8.img
	qemu-system-aarch64 -M raspi3b -serial stdio -kernel kernel8.img

clean:
	rm -f *.o *.elf *.img