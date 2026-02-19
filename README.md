# AI Direct-to-Metal: YOLO en Raspberry Pi Zero 2 W sin SO

**Hipótesis:** Un modelo de detección de objetos corre significativamente más rápido sin sistema operativo, hablando directamente con el hardware ARM.

**Resultado hasta hoy:** El mismo modelo TinyYOLO en la misma placa corre **~67× más rápido** en bare-metal 4-core NEON que en Linux + Python + PyTorch (estimado).

---

## Estado actual de rendimiento (hardware real, RPi Zero 2 W)

| Implementación | Latencia | FPS | vs Baseline |
|---|---|---|---|
| Linux + Python + PyTorch (estimado) | ~5,000 ms | ~0.2 | 1× baseline |
| Bare Metal — C++ sin MMU/caché | ~6,900 ms | ~0.14 | 0.5× (overhead > beneficio sin caché) |
| Bare Metal — C++ + MMU + D-Cache | 222 ms | 4.5 | ~22× |
| Bare Metal — NEON 4ch single-core | ~129 ms est. | ~7.7 | ~39× |
| Bare Metal — **4-core NEON + MMU + D-Cache** | **75 ms** | **13** | **~67×** |

> Los tiempos de hardware real se miden con el ARM Generic Timer (`cntpct_el0` a 19.2 MHz).

---

## Fases del proyecto

| Fase | Objetivo | Estado | Resultado clave |
|:---:|---|:---:|---|
| **1** | Acelerar operaciones matemáticas (GEMM) | ✅ | NEON ASM: **2.52×** vs GCC -O3 |
| **2** | Video sin drivers de SO | ✅ | Framebuffer via GPU Mailbox, 1920×1080 |
| **3** | Pipeline de inferencia TinyYOLO completo | ✅ | 3 Conv+BN+ReLU → MaxPool → Head → NMS |
| **4** | MMU + Cache → NEON generalizado | ✅ | 222ms → NEON 4ch = **~100ms** |
| **5** | 4-core paralelo | ✅ | `parallel_conv2d` → **~30ms est. en HW real** |
| **6** | Entrenamiento real + cámara CSI-2 | ⏳ | Siguiente fase |
| **6** | Entrada de cámara (MIPI CSI-2) | ⏳ | Hardware pendiente |

---

## Arquitectura del stack "Zero-Layer"

```
┌─────────────────────────────────────────────────┐
│  TinyYOLO: 64×64×3 → bboxes                    │
├─────────────────────────────────────────────────┤
│  Inference Engine (kernel.cpp)                  │
│  ┌──────────┐ ┌──────────┐ ┌─────────────────┐ │
│  │conv2d    │ │BatchNorm │ │YOLO decode      │ │
│  │NEON 4ch  │ │NEON vect │ │sigmoid/exp/NMS  │ │
│  └──────────┘ └──────────┘ └─────────────────┘ │
├─────────────────────────────────────────────────┤
│  MMU + D-Cache + I-Cache (mmu.cpp)              │
│  RAM: Normal WB Cached | MMIO: Device-nGnRnE    │
├─────────────────────────────────────────────────┤
│  Hardware: BCM2837 Cortex-A53 @ 1GHz / 512MB   │
└─────────────────────────────────────────────────┘
```

Sin sistema operativo, sin libc, sin malloc, sin Python.

---

## Cómo construir y probar

### Requisitos

- Docker (para el entorno de compilación cruzada)
- QEMU (para simulación)
- Python 3 + PyTorch (solo para exportar pesos)

### Build

```bash
# Construir la imagen Docker (una vez)
docker build -t rpi-forge .

# Exportar pesos (requiere PyTorch en el host)
python3 tools/export_model.py

# Compilar el kernel
docker run --rm -v $(pwd):/app rpi-forge make kernel8.img

# Limpiar y recompilar
docker run --rm -v $(pwd):/app rpi-forge make clean && \
docker run --rm -v $(pwd):/app rpi-forge make kernel8.img
```

### Simulación en QEMU

```bash
docker run --rm -v $(pwd):/app rpi-forge \
  qemu-system-aarch64 -M raspi3b -kernel kernel8.img -serial stdio -display none
```

Salida esperada:
```
=== BOOT RPI ZERO 2 W (DIAGNOSTICS) ===
Current EL: 1
SCTLR_EL1 (After): MMU ON | D-Cache ON | I-Cache ON
=== PERFORMANCE BENCHMARK ===
L1 (Conv 3->16):    XX ms
TOTAL TIME:         XX ms
FPS:                XX
```

### Hardware real (SD card)

```bash
# Copiar kernel8.img a la SD
cp kernel8.img /media/$USER/boot/

# Conectar serial USB-TTL a GPIO14(TX)/GPIO15(RX), GND
# Abrir terminal a 115200 baud
minicom -D /dev/ttyUSB0 -b 115200
```

### Benchmark de comparación (en Pi con Linux)

```bash
pip install torch --index-url https://download.pytorch.org/whl/cpu
python3 tools/benchmark_pytorch.py
```

---

## Estructura del repositorio

```
Direct2Metal/
├── src/
│   ├── start.s          # Bootloader: EL2→EL1, BSS clear, stack init
│   ├── kernel.cpp       # Main: UART, MMU, benchmark, framebuffer
│   ├── mmu.cpp          # MMU Identity Map + cache enable
│   ├── ops.h / ops.cpp  # Conv2d NEON, BN, ReLU, MaxPool, YOLO decode
│   ├── conv2d.cpp       # Dispatcher: conv2d_neon_4ch / conv2d_cpp
│   ├── conv2d_neon.s    # NEON 3×3 kernel (legacy, 4 output ch)
│   ├── matmul_neon.s    # NEON GEMM (Phase 1)
│   ├── data.s           # .incbin weights.bin + test_image.bin
│   ├── linker.ld        # Memory layout: .text .rodata .data .bss
│   ├── model_config.h   # Dimensiones y offsets de pesos (auto-generado)
│   ├── weights.bin      # Pesos TinyYOLO en formato NEON repacked
│   └── test_image.bin   # Imagen de prueba 64×64×3 CHW float32
├── tools/
│   ├── export_model.py      # Exporta pesos desde PyTorch + repacking NEON
│   ├── benchmark_pytorch.py # Baseline: TinyYOLO en PyTorch para comparar
│   └── reference_output.txt # Salida de referencia PyTorch (384 valores)
├── Makefile
└── Dockerfile
```

---

## Decisiones técnicas clave

### Formato de tensores: CHW (Channel-first)
- `buf[c][y][x] = buf[c*H*W + y*W + x]`
- Más eficiente para convolución: un canal = bloque contiguo en memoria

### Pesos NEON repacked: `[C_out/4][C_in×K×K][4]`
- Los 4 pesos de los 4 canales de salida quedan contiguos → `vld1q_f32` único
- `acc += vld1q_f32(w_ptr) × input_scalar` con `vmlaq_n_f32`

### MMU con T0SZ=34 (1 GB, Level-2 table)
- RAM: Normal WB Cached, Inner Shareable
- MMIO (0x3F000000+): Device-nGnRnE, Execute Never
- **Bug crítico resuelto:** T0SZ=25 mapeaba los periféricos como RAM cached → UART silenciado

### BatchNorm pre-fusionado
- `scale[c] = gamma[c] / sqrt(var[c] + eps)`, `bias[c] = beta[c] - mean[c]*scale[c]`
- Sin división ni sqrt en inferencia, solo `fmla`

### Buffers estáticos en BSS (sin malloc)
- `feat_a[16×64×64]`, `feat_b[32×32×32]`, `feat_c[64×16×16]`... ~514 KB total
- BSS zerofill en boot por `start.s`
