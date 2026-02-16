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
    *   **Goal:** Prove that hand-written assembly code can beat a C++ compiler for matrix multiplication, a core operation in neural networks.
    *   **Result:** Achieved a **2.52x speedup** compared to the GCC compiler's best optimization (-O3). This proves the core concept is viable.

2.  **Phase 2: Video Output (Complete)**
    *   **Goal:** Display an image on a screen without using any operating system drivers.
    *   **Result:** Successfully initialized the Raspberry Pi's GPU and wrote pixels to the screen's framebuffer.

3.  **Phase 3: AI Model Integration (In Progress)**
    *   **Goal:** Convert a pre-trained YOLO model into a format that can be executed on our bare-metal engine.

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
*   **Key Technologies:**
    *   **NEON:** ARM's 128-bit SIMD (Single Instruction, Multiple Data) architecture for accelerated computing.
    *   **Mailbox Interface:** A low-level communication channel to interact with the Raspberry Pi's VideoCore GPU.
