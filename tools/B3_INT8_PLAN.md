# B3 — INT8 Quantization Plan

> Estado: pendiente. Three tiers ranked por trade-off ganancia/esfuerzo.

## Estado del repo (2026-05-11)

| Archivo | Qué es | Status |
|:---|:---|:---|
| `tools/export_int8.py` | Convierte YOLOv5n.pt → INT8 weights con per-tensor sym scale | **Ya existe**, validado contra `ultralytics/yolov5` hub |
| `src/weights_int8.bin` | Output del script (1.88 MB) | **Ya existe**, pero no está cargado por el kernel |
| `src/weights.bin` | Pesos FP32 (7.47 MB) | En uso |
| `src/data.s` | Asm que embebe `weights.bin` en `.rodata` vía `.incbin` | Apunta a FP32 |

Formato de `weights_int8.bin` (por layer Conv2d):
```
struct LayerINT8 {
    uint32_t n_weights;
    float    scale;          // per-tensor symmetric: w_fp = w_int * scale
    int8_t   weights[n_weights];   // NEON-packed transpose for K>1 + C_out%4==0
    uint32_t n_bias;
    float    bias[n_bias];   // bias stays FP32, added to accumulator at the end
};
```

---

## Tier 1 — W8A32 (weights-only)

**Ganancia esperada:** ×1.2–1.5
**Esfuerzo:** 2–3 días, 1 sesión
**Riesgo:** bajo (mismas activations, solo cambia weight load + scale multiply)

### Pasos
1. Cambiar `src/data.s` para `.incbin "weights_int8.bin"` en su propia sección,
   o agregar un segundo `.incbin` y exponer `weights_int8_start/_end`.
2. CRC en kernel.cpp: añadir `WEIGHTS_INT8_CRC32` y reescribir el verify.
3. Refactorizar `WeightStream` para leer el nuevo formato (n_weights, scale,
   bytes int8, n_bias, bias floats).
4. Nueva variante `parallel_conv1x1_w8a32`:
   - Carga `vld1q_s8(w_int8 + ci*8)` → 8 weight bytes
   - `vmovl_s8` → int16x8, `vmovl_s16` × 2 → int32x4 ×2
   - `vcvtq_f32_s32` → fp32, multiplica por `vdupq_n_f32(scale)`
   - Mismo MLA loop con `v_in` fp32
5. Variante similar para `parallel_conv2d_w8a32` (más compleja por K×K).
6. Bias suma al final del acumulador, como ya hace `write_layer_int8`.
7. SiLU y demás activaciones siguen FP32, sin cambios.

### Por qué solo ×1.2–1.5
El bottleneck FP32 actual NO es bandwidth de weights (es FMA throughput).
Reducir weight bandwidth ayuda en L1 cache pressure pero no acelera las
operaciones aritméticas. Vale la pena solo si las layers grandes (256 ch)
muestran que el L1 thrashing es significativo en el `[P]` UART output.

---

## Tier 2 — W8A8 (full integer, sin calibración cara)

**Ganancia esperada:** ×2–3
**Esfuerzo:** 1–2 semanas, 3–4 sesiones
**Riesgo:** medio (re-quant entre layers puede causar overflow/saturación)

### Pasos
1. **Calibración offline (sesión 1):**
   - Modificar `tools/export_int8.py` para quantizar también activaciones:
     - Per-tensor symmetric int8 con range = max|act| × 1.05 (margen 5%)
     - Activation scale por capa, escrita junto al weight scale en el bin
   - Capturar ~50 frames representativos en HW (UART o SD card) → 50 × 1.58 MB
   - Correr FP32 inference offline (Python + PyTorch), grabar min/max per layer
   - Output: `src/weights_int8.bin` aumentado con `act_scale_in`, `act_scale_out`
     por capa.

2. **Kernels INT8 (sesión 2):**
   - `parallel_conv1x1_w8a8`:
     - Load input int8: `vld1q_s8` (16 bytes = 16 channels at one position)
     - Load weight int8: `vld1q_s8` (16 weights for one ci across 16 co)
     - `vmull_s8` → int16x8, accumulate to int32x4 via `vmlal_s16`
     - A53 no tiene SDOT (armv8.2-A); A53 es armv8.0 puro
     - Add bias FP32 reconvertido, requantize: `acc_i32 * scale_w * scale_in / scale_out`
     - Clip to [-128, 127], store as int8
   - `parallel_conv2d_w8a8`: similar pero con K×K depthwise-friendly inner
   - `concat`, `upsample`, `maxpool` ya operan element-wise → trivial en int8
   - **SiLU**: LUT de 256 entradas (int8 → int8) precomputada vs `x * sigmoid(x)`

3. **Re-quant entre layers (sesión 3):**
   - Cada layer output = `(acc_int32 * w_scale * in_scale / out_scale) → clip → int8`
   - Sumamos overhead ~5% de re-quant pero ganamos 4× en mem bandwidth + 2-3× en FMA
   - Cuidar overflow del int32 accumulator (raro pero posible en convs profundas)

4. **Validación (sesión 4):**
   - Comparar bbox IoU vs FP32 con los mismos 50 frames de calibración
   - Si mAP cae > 5pts, hacer per-channel quantization en lugar de per-tensor

### Bias INT32
Bias en el formato actual es FP32. Para W8A8 puro conviene pre-multiplicar
bias por `1/(scale_in * scale_w)` y guardarlo como int32. Suma directa al
acumulador, sin floating point en el hot loop.

---

## Tier 3 — Custom A53-optimized layouts

**Ganancia esperada:** ×3–4 (sobre W8A8)
**Esfuerzo:** 2–3 semanas
**Riesgo:** alto

### Ideas
- **Re-layout weights** para `ldp` (load pair) doble-issue en NEON pipe del A53.
- **Cache blocking** de activaciones por L1 (32 KB) y L2 (512 KB compartidos).
- **Winograd F(2,3)** sobre INT16 accumulators (combina con W8A8).
- **Layer fusion** Conv+BN+SiLU+Add en un solo paso (ya parcialmente).

Solo vale la pena si la diferencia tras Tier 2 es perceptible al usuario.

---

## Recomendación

1. Ejecutar B0+B1+B2 que ya están listos y medir UART `[P]` per-stage.
2. Si `bb` (backbone L0→L8) domina (esperado), considerar W8A32 (Tier 1)
   para descomprimir L1 cache pressure.
3. Solo invertir en W8A8 (Tier 2) si después de Tier 1 sigues debajo de
   ~10 fps reales. La complejidad de re-quant y la posible pérdida de
   precisión no se justifican si ya estás cerca del techo aceptable.
4. Tier 3 es academic — no lo persigas hasta tener un caso de uso que
   lo demande explícitamente.
