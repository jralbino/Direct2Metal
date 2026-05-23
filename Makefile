# --- TOOLCHAIN ---
CC = aarch64-linux-gnu-gcc
CXX = aarch64-linux-gnu-g++
LD = aarch64-linux-gnu-ld
OBJCOPY = aarch64-linux-gnu-objcopy

# --- APPLICATION SELECTOR ---
# Step 5 (BSP/app split): one image per app. Override with `make APP=other`.
APP ?= yolo_v8n_coco

# --- WEIGHT PRECISION SELECTOR ---
# G2 Tier 1: `make USE_INT8=1` switches the model to W8A32 (int8 weights,
# fp32 activations + accumulators). Default 0 keeps the FP32 path.
USE_INT8 ?= 0
ifeq ($(USE_INT8),1)
    INT8_DEF = -DUSE_INT8_WEIGHTS
else
    INT8_DEF =
endif

# --- INCLUDE PATHS ---
INC = -Ibsp -Iruntime -Iapp/$(APP) $(INT8_DEF)

# --- FLAGS ---
CFLAGS = -O3 -g -Wall -nostdlib -nostartfiles -ffreestanding $(INC)
CXXFLAGS = -Wall -O3 -g -nostdlib -nostartfiles -ffreestanding \
           -mcpu=cortex-a53 -mtune=cortex-a53 \
           -funsafe-math-optimizations -ffast-math \
           -ftree-vectorize \
           -mno-outline-atomics \
           -Wno-array-bounds \
           $(INC) \
           -Werror=return-type

SIMFLAGS = $(CXXFLAGS) -DSIMULATION

# --- SOURCES ---
# Object files are flat at the project root; filenames are globally unique so
# no collision. VPATH below tells make where to find each source by basename.
BSP_ASM_SRCS  = bsp/start.s
BSP_CPP_SRCS  = bsp/kernel.cpp bsp/mmu.cpp bsp/multicore.cpp \
                bsp/mailbox.cpp bsp/video.cpp bsp/watchdog.cpp \
                bsp/camera.cpp bsp/camera_bsc.cpp bsp/camera_imx708.cpp \
                bsp/camera_unicam.cpp bsp/camera_debayer.cpp

RT_CPP_SRCS   = runtime/ops.cpp runtime/conv2d.cpp
RT_ASM_SRCS   = runtime/neon/matmul_neon.s runtime/neon/conv2d_neon.s

APP_ASM_SRCS  = app/$(APP)/data.s
APP_CPP_SRCS  = app/$(APP)/yolo_v8n.cpp app/$(APP)/hud.cpp

ASM_SRCS = $(BSP_ASM_SRCS) $(RT_ASM_SRCS) $(APP_ASM_SRCS)
CPP_SRCS = $(BSP_CPP_SRCS) $(RT_CPP_SRCS) $(APP_CPP_SRCS)

# Where to find sources by basename (filenames are globally unique).
VPATH = bsp:runtime:runtime/neon:app/$(APP)

# --- HARDWARE OBJECTS (flat at project root) ---
HW_OBJS = $(notdir $(ASM_SRCS:.s=.o)) $(notdir $(CPP_SRCS:.cpp=.o))

# --- SIMULATION OBJECTS (sim_ prefix to avoid stale .o conflicts) ---
SIM_OBJS = $(addprefix sim_,$(HW_OBJS))

# --- TARGETS ---
all: kernel8.img

sim: $(SIM_OBJS)
	$(LD) -T bsp/linker.ld -o kernel8_sim.elf $(SIM_OBJS)
	qemu-system-aarch64 -M raspi3b -kernel kernel8_sim.elf -serial stdio -display none

sim_elf: $(SIM_OBJS)
	$(LD) -T bsp/linker.ld -o kernel8_sim.elf $(SIM_OBJS)

# --- COMPILE RULES ---

# data.s incbins weights/test_image — rebuild data.o when any changes.
data.o sim_data.o: app/$(APP)/weights.bin app/$(APP)/weights_int8.bin app/$(APP)/test_image.bin

# Hardware ASM/C++ (VPATH-resolved).
%.o: %.s
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Simulation ASM/C++ — sim_ prefix, -DSIMULATION for C++.
sim_%.o: %.s
	$(CC) $(CFLAGS) -c $< -o $@

sim_%.o: %.cpp
	$(CXX) $(SIMFLAGS) -c $< -o $@

# --- LINK ---
kernel8.elf: bsp/linker.ld $(HW_OBJS)
	$(LD) -T bsp/linker.ld -o $@ $(HW_OBJS)

# --- FINAL IMAGE ---
kernel8.img: kernel8.elf
	$(OBJCOPY) -O binary $< $@

# --- CLEAN ---
clean:
	rm -f *.o *.elf *.img
