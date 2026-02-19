# --- TOOLCHAIN ---
CC = aarch64-linux-gnu-gcc
CXX = aarch64-linux-gnu-g++
LD = aarch64-linux-gnu-ld
OBJCOPY = aarch64-linux-gnu-objcopy

# --- FLAGS ---
# Incluimos -Isrc para que encuentre los .h en src/
CFLAGS = -O3 -Wall -nostdlib -nostartfiles -ffreestanding -Isrc
CXXFLAGS = -O3 -Wall -nostdlib -nostartfiles -ffreestanding -fno-exceptions -fno-rtti -Isrc -mno-outline-atomics

# --- SOURCES ---
# Lista explícita de tus archivos fuente
ASM_SRCS = src/start.s src/matmul_neon.s src/data.s src/conv2d_neon.s
CPP_SRCS = src/kernel.cpp src/ops.cpp src/conv2d.cpp src/mmu.cpp src/multicore.cpp

# --- OBJECTS ---
# Convertimos src/xxx.s -> xxx.o
OBJS = $(ASM_SRCS:src/%.s=%.o) $(CPP_SRCS:src/%.cpp=%.o)

# --- TARGETS ---
all: kernel8.img

# Regla para Ensamblador (.s -> .o)
%.o: src/%.s
	$(CC) $(CFLAGS) -c $< -o $@

# Regla para C++ (.cpp -> .o)
%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Regla para Enlazar (Linking)
kernel8.elf: src/linker.ld $(OBJS)
	$(LD) -T src/linker.ld -o $@ $(OBJS)

# Regla final (ELF -> IMG)
kernel8.img: kernel8.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f *.o *.elf *.img