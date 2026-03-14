# Direct2Metal — Plan de Desarrollo

> Última actualización: 2026-03-14
> Estado del proyecto: Phase 9 en curso — driver CSI-2 bare-metal IMX708, V93 pendiente de flash

---

## Resumen ejecutivo

El motor de inferencia (Phases 1–8) está completo y funcional: YOLOv5n 320×320 corriendo en bare-metal AArch64 a **511 ms / ~2 FPS** con 4 cores Cortex-A53, NEON SIMD, MMU + D-cache, y safety hardening ISO 26262.

El bloqueante actual es **Phase 9**: el driver CSI-2 bare-metal para la Pi Camera v3 (IMX708) no ha logrado capturar un frame todavía. Los cambios V90–V93 tienen identificado el root cause (CPM=CCP2 bug + IDI0=0x2B) pero están en el working tree sin commitear y sin probar en hardware.

---

## Track A — Desbloquear la cámara *(bloqueante)*

### A1 — Commit y flash V93

**Estado:** Cambios listos en working tree. Pendiente de commit + flash.

Tres fixes simultáneos identificados a través de V90–V93:

| Fix | Registro | Valor incorrecto | Valor correcto | Motivo |
|:----|:---------|:-----------------|:---------------|:-------|
| CPM=CSI-2 | `CTRL` | `0x00080F1B` (BIT(3)=CPM=1→CCP2) | `0x080F02/03` | BIT(3) no es "lane enable" — activa modo CCP2 que silencia el protocol engine |
| Data type | `IDI0` | `0x2A` (RAW8 filter) | `0x2B` (RAW10) | IMX708 no tiene modo RAW8; siempre emite DT=0x2B. IDI0=0x2A descarta 100% de los paquetes de pixel |
| Clock gate | `CLKGATE` | `0x5A000005` | `0x5A000015` | Shift+OR correcto para 2-lane; dirección `0x3F802000` (no `0x3F802004`) ya confirmada en V92 |

**Checklist de commit:**
- [ ] `CTRL = 0x080F02` (U_CTRL_BASE sin lane bits)
- [ ] `IDI0 = 0x2B`
- [ ] `CLKGATE = 0x5A000015` en `0x3F802000`
- [ ] `FRAME_W = 1920` (1536 px × 10 bit / 8 = 1920 bytes/línea)
- [ ] `FRAME_SZ = 1,658,880`
- [ ] Corregir comentarios stale en `hardware_sim.h` líneas 59–64 (BIT(3)/BIT(4) descritos como "lane enable bits" — descripción del modelo incorrecto de V90)
- [ ] Corregir `UnicamSimState.clkgate_enabled` comment (todavía dice `0x3F802004`)
- [ ] Validar en QEMU sim → ALL preconditions pass
- [ ] Flash en hardware

**Resultado esperado:** `CTRL=0x00080F03`, `STA > 0` (FS+FE bits set), `IBWP` avanzando más allá de `IBSA0`.

---

### A2 — Primer frame: validar STA y IBWP

Una vez flasheado V93, el criterio de éxito es:

```
STA  = 0x00000003   (ISTA_FS | ISTA_FE fired)
IBWP = IBSA0 + FRAME_SZ   (DMA completó el buffer)
```

Si `STA > 0` pero `IBWP` no avanza → revisar `IBEA0` y `IBLS` (stride).

Si `STA = 0` todavía → pasar a **Track C**.

---

### A3 — Debayer RAW10

`camera_debayer.cpp` actualmente asume RAW8 (1 byte/pixel). Con IDI0=0x2B el DMA recibe RAW10 packed: **4 pixels en 5 bytes** (MSBs + 4 pares de 2 LSBs).

**Cambios requeridos:**

```
Unpack RAW10:
  byte[0..3] = 8 MSBs de pixels P0..P3
  byte[4]    = {P3[1:0], P2[1:0], P1[1:0], P0[1:0]}

  P0_10bit = (byte[0] << 2) | ((byte[4] >> 0) & 0x3)
  P1_10bit = (byte[1] << 2) | ((byte[4] >> 2) & 0x3)
  P2_10bit = (byte[2] << 2) | ((byte[4] >> 4) & 0x3)
  P3_10bit = (byte[3] << 2) | ((byte[4] >> 6) & 0x3)
```

Implementar con `vld1q_u8` + shift/mask NEON para procesar 4 grupos (16 pixels) por iteración. La conversión a float32 puede hacerse directamente: `pixel_f32 = pixel_10bit * (1.0f / 1023.0f)`.

Verificar orientación del sensor tras primer frame (la conclusión sobre el cruce D0/D1 cambió entre V17 y V35 — no asumir la orientación hasta verla).

---

### A4 — Crop y resize al tensor YOLO

El pipeline ya contempla `1536×864 → center-crop 864×864 → bilinear resize 320×320 CHW float32`. Validar:

- Crop centrado correcto (offset x = 336 px)
- Resize bilinear o nearest neighbor (nearest es suficiente para detección, ~3× más rápido)
- Output layout `[3][320][320]` normalizado `[0.0, 1.0]`

---

### A5 — Loop cerrado: Camera → YOLO → HDMI

Reemplazar `test_image.bin` con `camera_capture_frame()` en el main loop:

```cpp
while (true) {
    watchdog_kick();
    if (g_use_camera) {
        camera_capture_frame(g_input_tensor);   // bloquea hasta ISTA_FE
    } else {
        memcpy(g_input_tensor, test_image, INPUT_SIZE);
    }
    run_yolo_complete(g_input_tensor);
    video_render_detections();
    flush_to_ram();
}
```

El `watchdog_kick()` debe ir **antes** de `camera_capture_frame()` para evitar timeout durante la espera de ISTA_FE (hasta ~18 ms a 55 fps del sensor).

---

## Track B — Optimizaciones de performance *(paralelo al Track A)*

Orden de prioridad por retorno sobre esfuerzo:

### B1 — INT8 post-training quantization ⚡ mayor impacto

**Ganancia estimada:** ~300 ms → total ~210 ms / **~4.7 FPS**
**Esfuerzo:** Alto (1–2 semanas)

Requiere dos etapas:

**Host (PyTorch):**
```python
# Calibration dataset: 100-200 imágenes COCO
model_int8 = torch.quantization.quantize_dynamic(model, {nn.Conv2d}, dtype=torch.qint8)
# Exportar escalas por capa a weights_int8.bin
```

**Bare-metal:**
- Reemplazar `float32x4_t` por `int8x16_t` en `conv2d_partial()` y `conv2d_partial_8ch()`
- Usar `vmull_s8` + `vpaddlq_s16` para accumulation
- Dequantize solo en las salidas de cada bloque C3/SPPF (no per-layer)
- Beneficio secundario: 4× reducción de ancho de banda en weights (7.5 MB → ~1.9 MB)

### B2 — conv1x1 8-ch

**Ganancia estimada:** ~10 ms
**Esfuerzo:** Bajo

Mismo patrón que `conv2d_partial_8ch()` ya implementado para K=3. Aplica a las detection-head layers 1×1 donde `C_out ≥ 32 && C_out % 8 == 0`. Cambio en `ops.cpp` + layout de pesos en `export_model.py`.

### B3 — PRFM prefetch en weight loads

**Ganancia estimada:** 5–15%
**Esfuerzo:** Medio

Insertar `prfm pldl1keep` 2–4 iteraciones adelante en el inner loop de weights de `conv2d_partial_8ch()`. La brecha entre speedup teórico 2× y medido 1.22× en el kernel 8-ch indica que el cuello de botella es el bandwidth LPDDR2 en weights de 7.5 MB — prefetch mitiga la latencia de cache miss sin cambiar el algoritmo.

```cpp
// En el inner loop, antes de usar w[pos*8]:
__builtin_prefetch(&w[(pos+3)*8], 0, 1);
```

### B4 — L0 stem K=6 unroll especializado

**Ganancia estimada:** ~5–10 ms
**Esfuerzo:** Bajo

El stem L0 tiene `C_in=3` fijo (RGB). Con `C_in` conocido en tiempo de compilación, el compilador puede hacer unroll completo del loop `ci` (3 iteraciones), eliminando el overhead de branch + contador. Crear una especialización:

```cpp
template<> void conv2d_partial<3, 6>(...)  // C_in=3, K=6
```

---

## Track C — Diagnóstico fallback *(solo si V93 falla en hardware)*

Si después de flashear V93 `STA` sigue en 0, seguir este orden de investigación:

### C1 — Volcar CTRL post-init

Verificar que CPM (BIT(3)) sea 0 después del `CPR pulse + CTRL_BASE`:

```
CTRL esperado: 0x00080F03
              [bits 20:12] OET=128  [bits 11:8] PFT=0xF  [bit 1] MEM  [bit 0] CPE
```

Si CPM=1, hay otro write posterior al CPR que está seteando BIT(3). Buscar en `camera_unicam.cpp` cualquier OR a CTRL después del paso de CPR.

### C2 — IDI0 bypass completo

Probar `IDI0 = 0x00` en modo loopback (acepta cualquier VC y DT). Si con IDI0=0x00 aparece STA>0 pero con IDI0=0x2B no, el sensor está enviando un data type diferente al esperado. Leer el valor de `0x0112/0x0113` por I2C después de `stream_on` para confirmar el DT activo.

### C3 — Inspección física del FFC

El FFC de la Pi Zero 2W para CSI es el componente más frágil del sistema. Un conector mal asentado pasa la prueba LP-11 (estados DC) pero falla en HS a 450 Mbps.

Checklist físico:
- [ ] FFC completamente insertado (click audible en ambos extremos)
- [ ] Orientación correcta: contactos metálicos hacia abajo en el conector de la Pi Zero 2W
- [ ] Ausencia de dobleces o marcas en el cable
- [ ] Probar con un FFC de reemplazo si hay disponible

### C4 — VPU mailbox UNICAM1 power-on

Verificar que el SET_POWER_STATE se está enviando con los parámetros correctos:

```
device_id = 0x0D  (UNICAM1/CSI-1)  ← NO 0x0C (UNICAM0/CSI-0)
state     = 0x01  (powered on)
mbox[4]   = 0x00  (req_resp_indicator, NO el buffer size)
```

Y que el flush DC CIVAC del buffer del mailbox ocurre antes del call al VPU (V87).

---

## Deuda técnica

| ID | Archivo | Descripción | Prioridad |
|:---|:--------|:------------|:----------|
| T1 | `hardware_sim.h:59-64` | Comentarios de BIT(3)/BIT(4) describen modelo incorrecto de V90 ("lane enable bits"). Real = CPM/CCP2 bits | Alta |
| T2 | `hardware_sim.h` | `UnicamSimState.clkgate_enabled` comment dice `0x3F802004` en lugar de `0x3F802000` | Media |
| T3 | `camera_unicam.cpp` | Constante `FRAME_W` hardcodeada como RAW8. Parametrizar por data type (RAW8 vs RAW10) | Media |
| T4 | `safety_config.h` | `CAMERA_DATA_TYPE` no existe como named constant — magic number en IDI0 write | Baja |

---

## Tabla de métricas objetivo

| Métrica | Estado actual | Sin INT8 + cámara | Con INT8 + cámara |
|:--------|:-------------:|:-----------------:|:-----------------:|
| FPS (inferencia) | ~2 FPS | ~2 FPS | ~4.7 FPS |
| Latencia P95 | ~511 ms | ~511 ms | ~215 ms |
| Fuente de imagen | `test_image.bin` | Camera v3 live | Camera v3 live |
| FPS pipeline total | — | ~1.8 FPS* | ~4.2 FPS* |

*Estimado: captura IMX708 a 55 fps no es el bottleneck; la inferencia sí.

---

## Precondición global

> **Toda versión nueva debe validarse en QEMU sim antes de flashear.**
> El simulador `hardware_sim.h` tiene cobertura de los bugs críticos (CPR ordering, ANA power-up, IDI0 filter, ICTL LIP). Un ALL PASS en QEMU no garantiza éxito en hardware, pero un FAIL en QEMU sí garantiza fallo en hardware.
