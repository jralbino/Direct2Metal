# --- TOOLCHAIN ---
CROSS   ?= aarch64-linux-gnu-
CC       = $(CROSS)gcc
CXX      = $(CROSS)g++
LD       = $(CROSS)ld
OBJCOPY  = $(CROSS)objcopy

# --- APPLICATION SELECTOR ---
# Step 5 (BSP/app split): one image per app. Override with `make APP=other`.
APP ?= yolo_v8n_coco

# --- WEIGHT PRECISION SELECTOR ---
# G2 Tier 1: `make USE_INT8=1` switches the model to W8A32 (int8 weights,
# fp32 activations + accumulators). Default 0 keeps the FP32 path.
# G2 Tier 2: `make USE_INT8_W8A8=1` switches to full W8A8 (int8 weights +
# int8 activations + int32 accumulators + fp32 out-stage). Mutually
# exclusive with USE_INT8=1; if both are set W8A8 wins.
USE_INT8 ?= 0
USE_INT8_W8A8 ?= 0
W8A8_DEBUG ?= 0
ifeq ($(USE_INT8_W8A8),1)
    INT8_DEF = -DUSE_INT8_W8A8
    ifeq ($(W8A8_DEBUG),1)
        INT8_DEF += -DW8A8_DEBUG
    endif
else ifeq ($(USE_INT8),1)
    INT8_DEF = -DUSE_INT8_WEIGHTS
else
    INT8_DEF =
endif

# B4: frame-skip detection. `make FRAME_SKIP_N=N` runs YOLO inference
# every N frames; the other N-1 reuse the cached preds[] but re-render
# the camera + HUD at the capture rate. V187: default 4 (camera+HUD ~16 fps,
# boxes refresh every 4th frame). `make FRAME_SKIP_N=1` = infer every frame.
# The bench/capture images force N=1 (see SERFLAGS) — hwbench.py needs a
# [P]/[ABS] block on every frame it parses.
FRAME_SKIP_N ?= 4
SKIP_DEF = -DYOLO_INFER_EVERY_N=$(FRAME_SKIP_N)

# V181: `make DEBUG=1` enables the camera thumbnail in the canvas + the
# thumbnail repaint inside display_pump. Default 0 hides the thumbnail —
# the canvas shows only bboxes + IDs over solid black. HUD chrome animates
# in both modes.
DEBUG ?= 0
DEBUG_DEF = -DDEBUG=$(DEBUG)

# V186: `make SHOW_CAMERA=0` restores the V167 dark canvas. Default 1 paints the
# live camera frame (640×360, letterboxed at rows 60..419 — the same CAM_DISP_*
# area the bboxes use) under the boxes via camera_render_fullres(). Costs ~20 ms
# per frame single-core; the V164 multi-core variant was removed in V167.
SHOW_CAMERA ?= 1
SHOW_DEF = -DSHOW_CAMERA=$(SHOW_CAMERA)

# V188: `make AWB=0` freezes the ISP white balance at the tuning-file daylight
# gains (WB_R=535 WB_B=455 Q8). Default 1 = grey-world AWB once per frame in
# bsp/camera_debayer.cpp (debayer_awb_update), which fixes the magenta-burnt
# whites (R/B clipped at 255 while G is fine) under indoor light.
AWB ?= 1
AWB_DEF = -DAWB=$(AWB)

# --- INCLUDE PATHS ---
INC = -Ibsp -Iruntime -Iapp/$(APP) $(INT8_DEF) $(SKIP_DEF) $(DEBUG_DEF) $(SHOW_DEF) $(AWB_DEF)

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

# V183: runtime/conv2d.cpp + runtime/neon/*.s (Phase 1-2 legacy) removed — no callers.
RT_CPP_SRCS   = runtime/ops.cpp runtime/ops_int8.cpp

APP_ASM_SRCS  = app/$(APP)/data.s
APP_CPP_SRCS  = app/$(APP)/yolo_v8n.cpp app/$(APP)/hud.cpp

ASM_SRCS = $(BSP_ASM_SRCS) $(APP_ASM_SRCS)
CPP_SRCS = $(BSP_CPP_SRCS) $(RT_CPP_SRCS) $(APP_CPP_SRCS)

# Where to find sources by basename (filenames are globally unique).
VPATH = bsp:runtime:app/$(APP)

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
data.o sim_data.o: app/$(APP)/weights.bin app/$(APP)/weights_int8.bin \
                   app/$(APP)/weights_int8_w8a8.bin app/$(APP)/test_image.bin

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

# =====================================================================
#  V183 — Serial-boot / unattended-bench workflow  (tools/HWBENCH.md)
#
#  kernel8_serial.img : code-only image (-DSERIAL_BOOT, no data.s) streamed
#                       over UART by tools/hwbench.py each iteration (~5 s).
#  d2m_data.bin       : FP32 weights + test_image, lives on the SD, loaded by
#                       the VPU at 0x08000000 via `initramfs` (config.serial.txt).
#  raspbootin64.img   : 936 B serial chain-loader, goes on the SD AS kernel8.img.
#  bench              : reset (RTS->RUN) + upload + capture + parse + golden diff.
# =====================================================================
# Bench + capture images always infer every frame regardless of FRAME_SKIP_N:
# hwbench.py takes the last frame and expects its [P] buckets, per-layer profile
# and [ABS] fingerprint — skip frames print none of those.
SERFLAGS  = $(CXXFLAGS) -DSERIAL_BOOT -UYOLO_INFER_EVERY_N -DYOLO_INFER_EVERY_N=1
SER_OBJS  = $(addprefix ser_,$(filter-out data.o,$(HW_OBJS)))

ser_%.o: %.s
	$(CC) $(CFLAGS) -c $< -o $@

ser_%.o: %.cpp
	$(CXX) $(SERFLAGS) -c $< -o $@

kernel8_serial.img: bsp/linker.ld $(SER_OBJS)
	$(LD) -T bsp/linker.ld -o kernel8_serial.elf $(SER_OBJS)
	$(OBJCOPY) -O binary kernel8_serial.elf $@
	@echo "kernel8_serial.img = $$(stat -c %s $@) bytes"
serial-kernel: kernel8_serial.img

d2m_data.bin: app/$(APP)/weights.bin app/$(APP)/test_image.bin tools/pack_data.py
	python3 tools/pack_data.py

raspbootin: tools/raspbootin64/raspbootin64.img
tools/raspbootin64/raspbootin64.img:
	$(MAKE) -C tools/raspbootin64 CROSS=$(CROSS)

# One-time SD prep: stage everything that goes on the card into ./sdcard/.
sdcard: raspbootin d2m_data.bin
	mkdir -p sdcard
	cp tools/raspbootin64/raspbootin64.img sdcard/kernel8.img
	cp build/config.serial.txt sdcard/config.txt
	cp d2m_data.bin sdcard/d2m_data.bin
	@echo "sdcard/ ready — add RPi firmware files, copy all to the SD boot partition"

# One unattended iteration: rebuild the code image, flash, capture, parse.
PORT       ?= /dev/ttyUSB0
FRAMES     ?= 3
BENCHFLAGS ?=
bench: kernel8_serial.img
	python3 tools/hwbench.py --port $(PORT) --kernel kernel8_serial.img --frames $(FRAMES) $(BENCHFLAGS)

# V185: `make capture` — same serial-boot flow but with the CAMERA ON. On frame
# CAPTURE the kernel dumps the exact model input tensor over UART as base64;
# hwbench.py --capture writes it to cam_frame.bin. Then:
#   python3 tools/capture_to_test_image.py cam_frame.bin --preview cam_frame.png
#   make d2m_data.bin   (+ recopy to SD, regenerate GOLDEN/GOLDEN_ABS)
# Separate cap_ objects so the flag never leaks into the bench image.
# The capture image infers every frame (~1.3 s), so AE/AWB only update every
# AE_PERIOD=4 frames ≈ 5 s. From the cold-start CIT (1131) a bright room needs
# ~20 updates to get highlights under the 2 % saturation threshold (measured
# V188: 15 updates → 4.6 %), so CAPTURE=90. The per-update
# `[AE] cit= mean= sat= d=` trace is printed in this build only.
CAPTURE  ?= 90
CAPFLAGS  = $(SERFLAGS) -DCAPTURE_FRAME=$(CAPTURE)
CAP_OBJS  = $(addprefix cap_,$(filter-out data.o,$(HW_OBJS)))
cap_%.o: %.s
	$(CC) $(CFLAGS) -c $< -o $@
cap_%.o: %.cpp
	$(CXX) $(CAPFLAGS) -c $< -o $@
kernel8_capture.img: bsp/linker.ld $(CAP_OBJS)
	$(LD) -T bsp/linker.ld -o kernel8_capture.elf $(CAP_OBJS)
	$(OBJCOPY) -O binary kernel8_capture.elf $@
CAPFILE  ?= cam_frame.bin
capture: kernel8_capture.img
	python3 tools/hwbench.py --port $(PORT) --kernel kernel8_capture.img --frames $$(( $(CAPTURE) + 2 )) \
	    --timeout 400 --capture $(CAPFILE) --no-golden $(BENCHFLAGS)

# --- CLEAN ---
clean:
	rm -f *.o *.elf *.img
	$(MAKE) -C tools/raspbootin64 clean 2>/dev/null || true
