# Usamos una imagen base ligera de Ubuntu
FROM ubuntu:22.04

# Evitar que apt nos haga preguntas interactivas durante la instalación
ENV DEBIAN_FRONTEND=noninteractive

# Instalar 'make' y la toolchain de compilación cruzada para AArch64
RUN apt-get update && apt-get install -y \
    make \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    && rm -rf /var/lib/apt/lists/*

# Establecemos el directorio de trabajo donde se montará tu código
WORKDIR /app