CC = aarch64-linux-gnu-gcc
CXX = aarch64-linux-gnu-g++
OBJCOPY = aarch64-linux-gnu-objcopy

CFLAGS = -Wall -O2 -ffreestanding -nostdlib -mcpu=cortex-a53 -mno-outline-atomics -ftree-vectorize -MMD -MP
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
LDFLAGS = -nostdlib -T src/linker.ld -Wl,--build-id=none -Wl,--hash-style=sysv

SRCS_C = $(wildcard src/*.c)
SRCS_CXX = $(wildcard src/*.cpp)
SRCS_ASM = $(wildcard src/*.S)
OBJS = $(SRCS_ASM:.S=.o) $(SRCS_C:.c=.o) $(SRCS_CXX:.cpp=.o)
DEPS = $(OBJS:.o=.d)

all: kernel8.img

kernel8.img: kernel8.elf
	$(OBJCOPY) -O binary kernel8.elf kernel8.img

kernel8.elf: $(OBJS)
	$(CXX) $(LDFLAGS) $(OBJS) -o kernel8.elf

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f src/*.o src/*.d *.elf *.img

-include $(DEPS)