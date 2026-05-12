# --- TOOLCHAIN ---
CC = aarch64-linux-gnu-gcc
CXX = aarch64-linux-gnu-g++
LD = aarch64-linux-gnu-ld
OBJCOPY = aarch64-linux-gnu-objcopy

# --- FLAGS ---
CFLAGS = -O3 -g -Wall -nostdlib -nostartfiles -ffreestanding -Isrc
CXXFLAGS = -Wall -O3 -g -nostdlib -nostartfiles -ffreestanding \
           -mcpu=cortex-a53 -mtune=cortex-a53 \
           -funsafe-math-optimizations -ffast-math \
           -ftree-vectorize \
           -mno-outline-atomics \
           -Wno-array-bounds \
           -Isrc \
           -Werror=return-type

SIMFLAGS = $(CXXFLAGS) -DSIMULATION

# --- SOURCES ---
ASM_SRCS = src/start.s src/matmul_neon.s src/data.s src/conv2d_neon.s
CPP_SRCS = src/kernel.cpp src/ops.cpp src/conv2d.cpp src/mmu.cpp src/multicore.cpp \
           src/mailbox.cpp src/video.cpp src/watchdog.cpp \
           src/camera.cpp src/camera_bsc.cpp src/camera_imx708.cpp \
           src/camera_unicam.cpp src/camera_debayer.cpp \
           src/hud.cpp

# --- HARDWARE OBJECTS ---
HW_OBJS = $(ASM_SRCS:src/%.s=%.o) $(CPP_SRCS:src/%.cpp=%.o)

# --- SIMULATION OBJECTS (separate prefix to avoid stale .o conflicts) ---
SIM_ASM_OBJS = $(ASM_SRCS:src/%.s=sim_%.o)
SIM_CPP_OBJS = $(CPP_SRCS:src/%.cpp=sim_%.o)
SIM_OBJS     = $(SIM_ASM_OBJS) $(SIM_CPP_OBJS)

# --- TARGETS ---
all: kernel8.img

# Simulation build: recompile ALL objects with -DSIMULATION, then run QEMU
sim: $(SIM_OBJS)
	$(LD) -T src/linker.ld -o kernel8_sim.elf $(SIM_OBJS)
	qemu-system-aarch64 -M raspi3b -kernel kernel8_sim.elf -serial stdio -display none

# Build sim ELF without running (for inspection)
sim_elf: $(SIM_OBJS)
	$(LD) -T src/linker.ld -o kernel8_sim.elf $(SIM_OBJS)

# --- COMPILE RULES ---

# data.s incbins the weights/test_image blobs — track them as explicit
# prerequisites so `make` rebuilds data.o when either changes.
data.o sim_data.o: src/weights.bin src/test_image.bin

# Hardware ASM (.s -> %.o)
%.o: src/%.s
	$(CC) $(CFLAGS) -c $< -o $@

# Hardware C++ (.cpp -> %.o)
%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Simulation ASM (.s -> sim_%.o)  — no -DSIMULATION needed for ASM
sim_%.o: src/%.s
	$(CC) $(CFLAGS) -c $< -o $@

# Simulation C++ (.cpp -> sim_%.o) — adds -DSIMULATION
sim_%.o: src/%.cpp
	$(CXX) $(SIMFLAGS) -c $< -o $@

# --- LINK ---
kernel8.elf: src/linker.ld $(HW_OBJS)
	$(LD) -T src/linker.ld -o $@ $(HW_OBJS)

# --- FINAL IMAGE ---
kernel8.img: kernel8.elf
	$(OBJCOPY) -O binary $< $@

# --- CLEAN ---
clean:
	rm -f *.o *.elf *.img
