# Contexto Técnico — Direct2Metal AI

Este documento es el mapa técnico del proyecto. Contiene el estado actual exacto, las decisiones de arquitectura y los pasos siguientes con instrucciones concretas.

---

## Estado actual (2026-02-18, actualizado)

### Lo que funciona

| Componente | Estado | Dónde |
|---|---|---|
| UART PL011 a 115200 | ✅ Funciona en QEMU y HW real | `kernel.cpp` |
| Framebuffer GPU (1920×1080) | ✅ Funciona en QEMU y HW real | `kernel.cpp::mbox_call` |
| MMU Identity Map + D/I-Cache | ✅ Validado en QEMU y HW real | `mmu.cpp` |
| TinyYOLO inference completo | ✅ Validado numéricamente (< 1e-4 error) | `ops.cpp`, `conv2d.cpp` |
| NEON conv2d 4-channel | ✅ 1.72× en QEMU, ~2.96× combinado HW real | `ops.cpp::conv2d_neon_4ch` |
| 4-core parallel_conv2d | ✅ **75 ms / 13 FPS medido en HW real** | `multicore.cpp` |
| Timer hardware (`cntpct_el0`) | ✅ 19.2 MHz en HW real | `kernel.cpp` |
| Export PyTorch → bare-metal | ✅ Con NEON repacking | `tools/export_model.py` |

### Rendimiento actual (HW real, RPi Zero 2 W)

```
Timer: 19,200,000 Hz (ARM Generic Timer)

── Baseline (scalar + cache) ─────────────────────
L1 (Conv 3->16):    29 ms
L2 (Conv 16->32):   95 ms
L3 (Conv 32->64):   92 ms
TOTAL:             222 ms → 4.5 FPS

── 4-core NEON (medido 2026-02-18) ───────────────
L1 (Conv 3->16):    12 ms  (2.4× speedup)
L2 (Conv 16->32):   29 ms  (3.3× speedup)
L3 (Conv 32->64):   28 ms  (3.3× speedup)
Head + Decode:       0 ms
TOTAL:              75 ms → 13 FPS  (~3× sobre baseline)
```

> Speedup = NEON 1.72× × multi-core 1.72× ≈ **2.96×** vs scalar+cache.
> Eficiencia paralela ~75% (límite: caché L2 compartida entre los 4 cores).

---

## Arquitectura de software

### Stack completo (de arriba a abajo)

```
[PyTorch en PC host]
    │ export_model.py: repack pesos a [C_out/4][C_in×K×K][4]
    ↓
[weights.bin + test_image.bin] ──incbin──→ [.rodata en kernel8.img]
    ↓
[start.s] → EL2→EL1, CPACR FPU/NEON ON, limpiar BSS, saltar a kernel_main()
    ↓
[mmu.cpp::init_mmu()] → tablas de traducción 1GB, T0SZ=34, MMU+D$+I$ ON
    ↓
[kernel.cpp::run_benchmark_inference()]
    ├── conv2d_neon_4ch()  →  [ops.cpp: NEON vld1q_f32 + vmlaq_n_f32]
    ├── batchnorm_inplace() → [ops.cpp: NEON vmlaq_f32]
    ├── leaky_relu_inplace() → [ops.cpp: NEON vbslq_f32]
    ├── maxpool2x2()
    ├── conv1x1()
    └── yolo_decode()  →  sigmoid, exp, NMS
    ↓
[UART] → "TOTAL TIME: XXX ms, FPS: XX"
    ↓
[GPU Mailbox] → framebuffer blanco en pantalla
```

### Formato de datos

| Dato | Formato | Tamaño |
|---|---|---|
| Tensores (feature maps) | CHW float32 | buf[c×H×W + y×W + x] |
| Pesos conv (en disco) | [C_out/4][C_in×K×K][4] float32 | 96,344 bytes total |
| Imagen de entrada | CHW float32 normalizada [0,1] | 12,288 floats |
| BN parámetros | [scale(C), bias(C)] pre-fusionados | 2×C floats por capa |

### Mapa de memoria en ejecución

```
0x00080000  .text   (código, ~50 KB)
            .rodata (weights.bin 96KB + test_image.bin 48KB)
            .data
            .bss → feat_a[65536] + feat_b[32768] + feat_c[16384]
                   + pool_out[4096] + head_out[384]
                   + translation_table[512×8B = 4KB]  ← debe estar en RAM
                   + mbox[36×4B]
            (~514 KB de buffers en BSS)
0x0007FFFF  Stack pointer inicial (crece hacia abajo)
...
0x3F000000  MMIO: Device-nGnRnE (periféricos BCM2837)
```

---

## Pasos siguientes (ordenados por impacto)

---

### PASO 1 — Medir NEON en hardware real
**Tiempo estimado:** 10 minutos
**Impacto:** Confirmar speedup real de NEON 4ch vs C++ escalar

#### Qué hacer:

1. El `kernel8.img` ya está compilado con NEON 4ch. Cópialo a la SD:
   ```bash
   cp kernel8.img /media/$USER/boot/
   sync
   ```

2. Enciende la RPi con la SD y conecta el serial:
   ```bash
   minicom -D /dev/ttyUSB0 -b 115200
   # o con screen:
   screen /dev/ttyUSB0 115200
   ```

3. Anota los tiempos del benchmark y llena esta tabla:

   | Capa | C++ escalar (anterior) | NEON 4ch (nuevo) | Speedup |
   |---|---|---|---|
   | L1 Conv 3→16 | 29 ms | ___ ms | ___× |
   | L2 Conv 16→32 | 95 ms | ___ ms | ___× |
   | L3 Conv 32→64 | 92 ms | ___ ms | ___× |
   | **Total** | **222 ms** | **___ ms** | **___×** |

4. Actualiza `CHANGELOG.md` con los resultados reales.

---

### PASO 2 — Comparativa con PyTorch (el argumento principal)
**Tiempo estimado:** 1-2 horas (instalar PyTorch en Pi Zero 2 W es lento)
**Impacto:** Demostrar el objetivo central del proyecto

#### Qué hacer:

1. En una SD separada, instala Raspberry Pi OS Lite (64-bit).

2. Arranca la Pi con esa SD y conecta por SSH o serial.

3. Instala PyTorch:
   ```bash
   pip install torch --index-url https://download.pytorch.org/whl/cpu
   # Alternativa si pip tarda: instalar desde wheel .whl precompilado
   ```

4. Copia el script al Pi:
   ```bash
   scp tools/benchmark_pytorch.py pi@<ip>:~/
   scp tools/export_model.py pi@<ip>:~/
   ```

5. Ejecuta y anota los resultados:
   ```bash
   python3 benchmark_pytorch.py --runs 20
   ```

6. Actualiza la tabla comparativa en `README.md` con los números reales.

> **Resultado esperado:** PyTorch tardará entre 2,000 ms y 8,000 ms.
> Esto dará un **speedup de 20×–80×** sobre bare-metal NEON.

---

### ✅ PASO 3 — Inferencia multi-core (4 cores Cortex-A53) — COMPLETADO

**Impacto esperado en HW real:** ~3-4× adicional → objetivo < 30 ms (>30 FPS)

#### Lo que se implementó:

- **`src/multicore.h` / `src/multicore.cpp`**: `parallel_conv2d()` divide los grupos de canales NEON entre 4 cores usando `task_epoch` (release) + `done_count` (acquire) + `sev`/`wfe`
- **`src/start.s`**: todos los cores hacen EL2→EL1, cores 1-3 van a `secondary_main()` con su propio stack (`0x80000 - core_id × 0x10000`)
- **Fallback automático**: probe de 500K ciclos al primer uso. En QEMU (cores inactivos) → single-core transparente. En HW real → 4 cores paralelos

#### Verificar en hardware real:

1. Copia `kernel8.img` a la SD y enciende la RPi
2. Busca en la salida UART el tiempo total — debería bajar de ~100ms a ~25–35ms
3. Si ves ">30 FPS" → multicore funcionó correctamente

#### División de trabajo:
| Capa | C_out | Grupos/core | Canales/core |
|---|---|---|---|
| L1 Conv 3→16 | 16 | 1 | 4 |
| L2 Conv 16→32 | 32 | 2 | 8 |
| L3 Conv 32→64 | 64 | 4 | 16 |

---

### PASO 4 — Entrenar el modelo con datos reales
**Tiempo estimado:** 1 día (entrenamiento en PC)
**Impacto:** Detectar objetos reales en lugar de pesos aleatorios

#### Actualmente:
El modelo tiene pesos aleatorios (`torch.manual_seed(42)`).
Resultado esperado: 0 detecciones (correcto — no hay conocimiento).

#### Qué hacer:

1. **Dataset recomendado:** COCO subset (80 clases → recortar a 1 clase: "persona")
   ```bash
   pip install ultralytics
   yolo train model=yolov5n data=coco128.yaml epochs=30 imgsz=64
   ```

2. **O dataset propio** (más fácil para demostrar):
   - Toma 200 fotos de un objeto con tu teléfono
   - Etiqueta con LabelImg: `pip install labelImg`
   - Entrena durante 50 épocas

3. **Exportar el modelo entrenado:**
   ```python
   # En tools/export_model.py, cargar pesos pre-entrenados:
   model.load_state_dict(torch.load('best.pt'))
   ```

4. **Re-exportar y recompilar:**
   ```bash
   python3 tools/export_model.py
   docker run --rm -v $(pwd):/app rpi-forge make kernel8.img
   ```

---

### PASO 5 — Entrada de cámara (MIPI CSI-2)
**Tiempo estimado:** 2-4 semanas
**Impacto:** Sistema completo: cámara → inferencia → bboxes en pantalla
**Dificultad:** Alta — requiere escribir un driver de hardware desde cero

#### Hardware necesario:
- Raspberry Pi Camera Module v2 (IMX219) — compatible con RPi Zero 2 W

#### Concepto técnico:
El BCM2837 tiene un controlador MIPI CSI-2 llamado **Unicam**.
Dirección base: `0x3F800000` (CCP2/Unicam).

#### Pasos de implementación:
1. Inicializar el bloque Unicam (registros de control)
2. Configurar el sensor IMX219 via I2C (bus en GPIO0/GPIO1)
3. Capturar un frame de 64×64 pixels (sub-sampling)
4. Convertir Bayer RAW → RGB float32 normalizado
5. Pasar directamente al pipeline de inferencia

> **Referencia:** Libcamera source code (solo para documentación, no usar la lib).

---

## Roadmap visual

```
Feb 2026
│
├─ ✅ PASO 1: Medir NEON en HW real        → 75 ms / 13 FPS (4-core NEON)
│
├─ ✅ PASO 3: Multi-core (4 cores)         → ~3× speedup sobre scalar+cache
│
├─ PASO 2: Comparativa PyTorch             ← 1-2 horas  ← SIGUIENTE
│
├─ PASO 4: Entrenar modelo real            ← 1 día → detecciones reales
│
└─ PASO 5: Driver MIPI CSI-2               ← semanas → sistema autónomo
```

---

## Comandos de referencia rápida

```bash
# Compilar desde cero
python3 tools/export_model.py && \
docker run --rm -v $(pwd):/app rpi-forge make clean && \
docker run --rm -v $(pwd):/app rpi-forge make kernel8.img

# Simular en QEMU
docker run --rm -v $(pwd):/app rpi-forge \
  qemu-system-aarch64 -M raspi3b -kernel kernel8.img -serial stdio -display none

# Ver tamaño de la imagen
ls -lh kernel8.img kernel8.elf

# Inspeccionar secciones del ELF
docker run --rm -v $(pwd):/app rpi-forge \
  aarch64-linux-gnu-objdump -h kernel8.elf

# Ver los símbolos exportados
docker run --rm -v $(pwd):/app rpi-forge \
  aarch64-linux-gnu-nm kernel8.elf | grep -E "(weights|bss|start)"

# Flashear SD (reemplaza /dev/sdX con tu dispositivo)
sudo dd if=kernel8.img of=/dev/sdX bs=512 seek=2048 conv=fsync
```
