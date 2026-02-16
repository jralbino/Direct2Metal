# File: Dockerfile
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

# Install Cross-Compiler (GCC AND G++), Make, QEMU
RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    build-essential \
    qemu-system-arm \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
CMD ["/bin/bash"]