# Project: AI Direct-to-Metal (YOLO on Raspberry Pi Zero 2 W)

**Goal:** Run a real-time object detection model (YOLO) on a Raspberry Pi Zero 2 W *without an operating system*.

This project explores whether we can achieve better performance for AI models by bypassing traditional software layers (like Linux, Python, and drivers) and communicating directly with the hardware.

## The Core Idea

Modern software is built on layers of abstraction. While these layers make development easier, they can also create overhead and limit performance, especially on resource-constrained devices.

This project's hypothesis is that by writing code that speaks the hardware's native language (ARM assembly), we can create a highly-optimized AI engine that outperforms standard, high-level implementations.

We are trading development convenience for raw performance.

## Project Phases

This project is divided into four main phases:

1.  **Phase 1: Accelerated Math (Complete)**
    *   **Goal:** Prove that hand-written assembly code can beat a C++ compiler for matrix multiplication.
    *   **Result:** Achieved a **2.52x speedup** over GCC -O3.

2.  **Phase 2: Video Output (Complete)**
    *   **Goal:** Display an image on a screen without using any operating system drivers.
    *   **Result:** Successfully initialized the Raspberry Pi's GPU and wrote pixels to the screen's framebuffer.

3.  **Phase 3: AI Model Integration (Complete)**
    *   **Goal:** Build a bare-metal inference engine for a key neural network operation (Conv2d).
    *   **Result:** The hand-written NEON Assembly kernel achieved a **1.75x speedup** over a naive C++ implementation.

4.  **Phase 4: Camera Input (Pending)**
    *   **Goal:** Capture images from a camera directly, again, without OS-level drivers.

## Getting Started

This project is designed to be built and run in a simulated environment using Docker and QEMU.

### Prerequisites

*   Docker
*   QEMU

### Building the Code

1.  **Clone the repository:**
    ```bash
    git clone <repository-url>
    cd <repository-name>
    ```

2.  **Build the Docker image:**
    ```bash
    docker build -t direct2metal-dev .
    ```

3.  **Run the build process:**
    ```bash
    docker run --rm -v $(pwd):/app direct2metal-dev make
    ```
    This will compile the C++ and assembly code and create a `kernel8.img` file.

### Running the Simulation

To run the compiled kernel in a simulated Raspberry Pi environment, use the following command:

```bash
qemu-system-aarch64 -M raspi3b -kernel kernel8.img -serial stdio
```

This will start the QEMU emulator, load our kernel, and display the output on the console.

## Technical Details

*   **Hardware:** Raspberry Pi Zero 2 W (ARM Cortex-A53)
*   **Emulator:** QEMU (`raspi3b` machine)
*   **Compiler:** `aarch64-linux-gnu-g++`

### The "Zero-Layer" Stack

The core of this project is a minimal software stack built from scratch:

*   **Bootloader (`start.s`):** A few lines of assembly that initialize the primary core (Core 0) and set up the stack.
*   **Kernel (`kernel.cpp`):** The main C++ orchestrator. It manages hardware communication (like the GPU Mailbox), loads model weights, and controls the main inference loop.
*   **AI Core (`conv2d_neon.s`):** A highly-optimized assembly kernel that performs the heavy lifting for 2D convolutions using ARM's NEON SIMD instructions. It uses a "Direct Pointer Access" strategy to avoid unnecessary memory copies and processes four output channels in parallel.
*   **GPU Communication (`mbox_call`):** A low-level communication channel to interact with the Raspberry Pi's VideoCore GPU for video initialization.
