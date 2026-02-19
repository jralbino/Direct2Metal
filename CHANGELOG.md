# Changelog — Direct2Metal AI

---

## [2026-02-18] — Fase 5: Multi-core (4 × Cortex-A53)

### Completado

**Infraestructura multi-core (`src/multicore.h` / `src/multicore.cpp`)**
- `parallel_conv2d()`: divide los grupos de canales de salida en 4 partes iguales (una por core)
- Protocolo de sincronización sin SO: `task_epoch` (release) + `done_count` (release/acquire) + `sev`/`wfe`
- Implementación de `conv2d_partial()`: misma aritmética NEON que `conv2d_neon_4ch()` pero para un rango de grupos
- `secondary_main()`: loop infinito de espera y ejecución en cores 1-3
- **Fallback automático**: probe con timeout de 500K ciclos al primer uso. En QEMU (cores inactivos) usa single-core NEON transparentemente; en HW real usa los 4 cores

**`src/start.s` renovado**
- Todos los cores pasan por la transición EL2→EL1 (no solo core 0)
- Cores 1-3 → `secondary_el1`: FPU/NEON habilitado, stack dedicado, espera `bss_ready`
- Flag `bss_ready` en `.data` (no `.bss`) → inmune a la carrera antes del clear de BSS
- Stacks por core: `sp = 0x80000 - core_id × 0x10000` (Core1: 0x70000, Core2: 0x60000, Core3: 0x50000)

**`Makefile`**: `-mno-outline-atomics` para evitar llamadas a `libgcc` en `-nostdlib`

### División de trabajo por capa

| Capa | C_out | Grupos NEON | Grupos por core | Canales por core |
|---|---|---|---|---|
| L1 Conv 3→16 | 16 | 4 | 1 | 4 |
| L2 Conv 16→32 | 32 | 8 | 2 | 8 |
| L3 Conv 32→64 | 64 | 16 | 4 | 16 |

### Benchmarks en hardware real (RPi Zero 2 W, timer 19.2 MHz)

| Capa | Scalar+Cache | 4-Core NEON | Speedup |
|---|---|---|---|
| L1 Conv 3→16 | 29 ms | **12 ms** | **2.4×** |
| L2 Conv 16→32 | 95 ms | **29 ms** | **3.3×** |
| L3 Conv 32→64 | 92 ms | **28 ms** | **3.3×** |
| **Total** | **222 ms** | **75 ms** | **~3×** |
| **FPS** | **4.5** | **13** | |

> Speedup ~3× vs scalar+cache = NEON 1.72× × multi-core 1.72× ≈ 2.96× ✓
> Eficiencia paralela ~75% (esperado: caché L2 compartida limita a <4×).

### Benchmarks en QEMU (fallback single-core)

| Configuración | Total | FPS |
|---|---|---|
| NEON 4ch single-core | 67 ms | 14 |
| NEON 4ch multi-core (QEMU fallback) | 67 ms | 14 |

> QEMU raspi3b no arranca cores secundarios → fallback automático, resultados idénticos.

---

## [2026-02-18] — Fase 4: MMU, Cache y NEON Conv2d Generalizado

### Completado

**MMU + Cache (mmu.cpp)**
- Implementada tabla de traducción de identidad de 1 GB (512 entradas Level-2 de 2 MB)
- RAM mapeada como Normal WB Cached, MMIO como Device-nGnRnE
- Bug crítico resuelto: `T0SZ=25` causaba que los periféricos se mapearan como RAM cached,
  silenciando el UART. Valor correcto: `T0SZ=34`
- Secuencia correcta: DSB → `tlbi vmalle1is` → ISB → MAIR/TCR/TTBR0 → ISB → SCTLR → ISB

**Pipeline TinyYOLO completo**
- 3 capas Conv2d (3→16→32→64) + BN pre-fusionado + LeakyReLU + MaxPool + YOLO head (Conv1×1)
- BatchNorm fusionado en export: `scale = gamma/sqrt(var+eps)`, `bias = beta - mean*scale`
- YOLO decode con `mini_expf`/`mini_sigmoidf` propios (sin libm)
- NMS con IoU > 0.45

**NEON conv2d generalizado (conv2d_neon_4ch)**
- Procesa 4 canales de salida simultáneamente con registros NEON de 128 bits
- Pesos reempacados a `[C_out/4][C_in×K×K][4]` → `vld1q_f32` único por posición de kernel
- `vmlaq_n_f32` en el bucle interno: `acc += 4_weights × 1_input_scalar`
- BN y LeakyReLU también vectorizados con `vmlaq_f32`, `vbslq_f32`

**start.s renovado**
- Transición EL2 → EL1 via `eret`
- CPACR_EL1.FPEN = 0b11 (habilita NEON/FP en EL1)
- Limpieza de BSS con `str xzr, [x0], #8` antes de saltar a C++

### Benchmarks en hardware real (RPi Zero 2 W, timer 19.2 MHz)

| Configuración | L1 | L2 | L3 | Total | FPS |
|---|---|---|---|---|---|
| C++ escalar sin caché | — | — | — | ~6,900 ms | 0.14 |
| C++ escalar + MMU/cache | 29 ms | 95 ms | 92 ms | **222 ms** | **4.5** |
| NEON 4ch + MMU/cache | *pendiente HW* | — | — | **~100 ms est.** | **~10** |

### Benchmarks en QEMU (timer virtual 62.5 MHz)

| Configuración | L1 | L2 | L3 | Total | FPS |
|---|---|---|---|---|---|
| C++ escalar + cache | 19 ms | 49 ms | 46 ms | 115 ms | 8 |
| NEON 4ch + cache | 11 ms | 28 ms | 28 ms | **67 ms** | **14** |

---

## [2026-02-16] — Fase 3: Pipeline de Inferencia TinyYOLO

### Completado
- Exportador PyTorch (`tools/export_model.py`): pesos, imagen de prueba, config header
- Operadores en C++ freestanding: batchnorm, leaky_relu, maxpool, conv2d, conv1x1, yolo_decode
- Validación numérica vs PyTorch: error máximo < 0.0001 (limitado por `uart_float`, 4 dígitos)
- Framebuffer GPU: resolución 1920×1080 detectada y framebuffer asignado correctamente

### Benchmarks
| Test | Implementación | Resultado |
|---|---|---|
| Conv2d (16×16) | C++ naive | 91,135 ciclos (baseline) |
| Conv2d (16×16) | NEON Direct Access | 51,933 ciclos → **1.75×** |

---

## [2026-02-15] — Fase 1 y 2: Matemáticas y Video

### Completado
- NEON GEMM 16×16: **2.52×** sobre GCC -O3 (16,106 → 6,383 ciclos)
- PL011 UART a 115200 baud (GPIO14/GPIO15 ALT0)
- GPU Mailbox property interface para framebuffer
- Entorno Docker + QEMU + toolchain aarch64-linux-gnu

---

## Tabla de benchmarks histórica

| ID | Test | Implementación | Resultado | Estado |
|:---:|---|---|:---:|:---:|
| B01 | GEMM 16×16 | C++ -O3 | 16,106 ciclos | Baseline |
| B02 | GEMM 16×16 | NEON ASM | 6,383 ciclos | **2.52×** |
| AI-01 | Conv2d 16×16 | C++ naive | 91,135 ciclos | Baseline |
| AI-02 | Conv2d 16×16 | NEON (copy) | 186,944 ciclos | FAIL |
| AI-03 | Conv2d 16×16 | NEON (direct) | 51,933 ciclos | **1.75×** |
| NN-01 | TinyYOLO 64×64 | C++ + cache | 222 ms | **4.5 FPS** |
| NN-02 | TinyYOLO 64×64 | NEON 4ch single-core | ~129 ms est. | **~7.7 FPS** |
| NN-03 | TinyYOLO 64×64 | 4-core NEON + cache | **75 ms** | **13 FPS** |
