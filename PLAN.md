# Direct2Metal — Plan de Desarrollo

> Última actualización: 2026-03-18
> Estado: Phase 9 activa — V111 listo para flash. **Imagen visible desde V108.** V109: captura PI0 funcional. V110: DMA stop/restart (29ms debayer vs 1100ms). V111: diagnósticos hex dump para depurar barras negras + monocromático.

---

## Resumen ejecutivo

El motor de inferencia (Phases 1–8) está completo: YOLOv5n 320×320 en bare-metal AArch64 a
**511 ms / ~2 FPS** con 4 cores Cortex-A53, NEON SIMD, MMU + D-cache, e ISO 26262 safety hardening.

Phase 9 (driver CSI-2 IMX708) — **Imagen visible desde V108. Captura funcional desde V109.**
V108: ROOT CAUSE (SET_DOMAIN_STATE faltante) → primer STA>0 en 21 versiones.
V109: PI0/FEI captura consistente (eliminado CPR-post-stream).
V110: DMA stop/restart tras captura — debayer bajó de 1100ms a 29ms. Orientación correcta.
V111: Diagnósticos hex dump (IBWP, raw bytes, RGGB pixel values) para depurar barras negras + monocromático.

**Issues pendientes V111:** barras negras + imagen monocromática. Posibles causas:
formato RAW10 packed vs unpacked, fase Bayer incorrecta, cache invalidation (DC CIVAC vs IVAC),
embedded data lines del sensor.

---

## Hallazgos clave V93–V109

| Versión | Cambio | Resultado | Conclusión |
|:--------|:-------|:----------|:-----------|
| V93 | CPM=0 (CSI-2), IDI0=0x2B, CLKGATE=0x5A000015 | STA=0 | Protocolo correcto; CLKGATE en dirección CSI0 (0x3F802000) — nunca llegó a CSI1 |
| V94 | 1-lane (0x0114=0), DAT1 disabled | STA=0 | Número de lanes descartado |
| V95 | CPR skipped, ANA=0x770 directo | STA=0 | CPR irrelevante; VPU NO calibra D-PHY |
| V96 | AR pulse + no CPR; fast-poll 1ms/50ms | STA_accum=0 en 50 muestras | Cero Frame Start events — CPE sordo al MIPI |
| V97/V98 | Settle sweep 1–5 ciclos a 100 MHz | STA=0 en todos | Settle timing NO es la causa raíz |
| V99 | CM_CAM1: 100 MHz → 250 MHz (DIVI=5→2) | STA=0 | Frecuencia del decoder NO es la causa |
| V100 | Power domain sweep + CPR post-stream | STA=0; CAM1=0x02 after SET(0x03); CPR borró ICTL/MISC | Power domain ambiguo; bug CPR descubierto |
| V101 | Fix ICTL/MISC post-CPR; ICTL=0x07 MISC=0x240 ✓ | STA=0 | Todos los registros correctos — espacio SW agotado |
| V102 | CMP0=0x80000301 restaurado | STA=0 | CMP0 descartado |
| A4 | Cable FFC re-seated | STA=0 | Cable físico descartado |
| A1 | Pi OS /dev/mem ground truth | — | CLK=0x0005, 2-lane, ICTL=0x00D80007, CM_CAM1=100MHz, IBWP=0xCBD95000=0xC0000000\|phys |
| V103 | Igualar Pi OS exactamente (CLK/DAT=0x0005, 2-lane, CM=100MHz, ICTL=0x00D80007) | STA=0 | Todos registros iguales. **Deshabilitó CLKGATE** basándose en lectura de CSI0 (0x3F802000) |
| V104 | IMX708 0x0310=0x00 (non-continuous HS clock) | STA=0 | Modo reloj MIPI descartado |
| ~~V105~~ | IBSA0 bus addr 0x40000000→0xC0000000 (VC bus alias) | STA=0 | Bus addr correcto pero no era causa. Kept (matches Pi OS) |
| V106 | CLKGATE @0x3F802004 (CSI1!) + 0x5A000015 | STA=0, readback=0x15 | Dirección corregida, pero no era suficiente solo |
| V107 | CLKGATE write post-CPE (orden Linux) | STA=0 | Orden correcto, falta algo más |
| **V108** | **SET_DOMAIN_STATE(14,1) + SET_CLOCK_RATE(4,250M)** | **STA=0xD001 ✓** | **ROOT CAUSE: firmware domain power faltante** |
| V109 | Eliminar CPR-post-stream + clear ISTA/STA | **PI0 captura ✓** | CPR destruía FE; sin CPR, PI0 detectado consistentemente en fast-poll |
| V110 | DMA stop/restart + DC CIVAC cache invalidation | **Orientación ✓, 29ms RGB** | DMA overwrite resuelto (CPE=0 tras captura). Barras negras + monocromático persisten |
| V111 | Diagnósticos: IBWP, hex dump, RGGB pixel values | **PENDIENTE FLASH** | Para determinar: formato datos, fase Bayer, cache effectiveness |

**ROOT CAUSE (V108): SET_DOMAIN_STATE faltante.**

Linux `bcm2835-unicam.c` usa `pm_runtime` → `raspberrypi-genpd` → `SET_DOMAIN_STATE`
(tag `0x00038030`, domain=14), NO el viejo `SET_POWER_STATE` (tag `0x00028001`, device=0x0d).
`SET_POWER_STATE` siempre devolvió state=0x02 (namespace de tags incorrecto).
Sin domain power, el decoder CSI-2 digital estaba congelado.

Resultado V108 en hardware:
- `STA_accum=0x0000D001` — Frame Start (BIT0) + PI0/CMP0 match (BIT15) por primera vez
- `ISTA=0x05` — FSI (Frame Start Int) + LCI (Line Capture Int)
- IBWP avanza de 0xC2240200 a 0xC238A200 — DMA recibiendo datos reales
- **Ruido visible en HDMI** — datos del sensor llegando al framebuffer
- FEI (BIT1) no dispara → timeout. Causa: CPR-post-stream diagnóstico destruyó detección FE

**⚠️ Incidente V108: pérdida y regeneración de código.**
Durante pruebas del simulador V108, un `git checkout src/camera_unicam.cpp` revirtió
el archivo a V93 (último commit), perdiendo TODOS los cambios V105–V108. El archivo se
regeneró manualmente a partir del contexto de la conversación. `hardware_sim.h` (V108)
sobrevivió. Lección: **hacer commit antes de ejecutar `git checkout` en archivos modificados.**

Hallazgos acumulados de versiones anteriores:
- **V96:** VPU deja `CTRL=STA=CLT=DLT=0x00000000`. `dtoverlay=imx708` no toca Unicam MMIO.
- **V100:** CPR borra ICTL (0x07→0x04) y MISC (0x240→0x000).
- **V106:** CSI0 CLKGATE=0x3F802000, CSI1 CLKGATE=0x3F802004 (Linux DT).

---

## Track A — Desbloquear la cámara *(bloqueante)*

> **Estado V111**: Captura funcional (PI0). Debayer RAW10 implementado (A5). DMA stop/restart funciona (29ms).
> **Problema abierto:** barras negras + imagen monocromática. V111 agrega diagnósticos para identificar causa raíz.

---

### ~~V102~~ — CMP0 restore ← COMPLETADO (FALSIFICADO)

CMP0=0x80000301 restaurado. Readback=0x80000101 (BIT(9) ignorado por hardware, normal).
STA=0. CMP0 no es la causa.

---

### ~~A4~~ — Inspección física FFC ← COMPLETADO (SIN EFECTO)

Un cable FFC con resistencia elevada en un contacto pasa estados DC (LP-11 = líneas en
reposo, ~1.2V) pero falla en AC a 690 Mbps. El receptor BCM2837 ve actividad HS (D0hi
cambia de 0x0A00 a 0xE000) pero el byte-stream tiene errores de bit que impiden el
SYNC 0xB8. Este síntoma es **idéntico** al STA=0 que observamos.

**Checklist:**
- [ ] Cable FFC completamente insertado — contacto audible en ambos extremos
- [ ] Orientación correcta — contactos metálicos mirando hacia abajo en Pi Zero 2W
- [ ] Sin doblez visible, sin microfractura en el centro del cable
- [ ] Probar con cable de reemplazo si disponible

---

### ~~V100~~ / ~~V101~~ — Power domain + CPR post-stream ← COMPLETADO (V100 hallazgos, V101 fix)

**Resultados V100:**
- Power domain: SD=0x01, UART/USB=0x00, CAM0/CAM1/DSP0=0x02. SET_POWER_STATE(CAM1,0x03)→0x02 (sin efecto).
  SD usa encoding diferente (0x01 no 0x03) → significado de 0x02 es AMBIGUO. No descartable sin /dev/mem.
- CPR post-stream: STA=0 antes y después. Bug: CPR borró ICTL (0x07→0x04) y MISC (0x240→0x000).

**Fix V101**: Restaurar ICTL=0x07, MISC|=0x240, re-assert CPE, trigger LIP después de CPR post-stream.
Confirmado en hardware: ICTL=0x00000007 MISC=0x00000240 ✓. STA=0 persiste.

---

### A1 — Ground truth: `/dev/mem` en Pi OS ← DEFINITIVO (requiere Pi OS)

Esta es la única acción que puede resolver el problema con certeza.
Comparar CADA registro con nuestros valores de V99 revelará la diferencia exacta.

```bash
# En Pi OS con cámara funcionando:
libcamera-vid -t 0 --nopreview &
sleep 2

sudo python3 -c "
import mmap, struct
with open('/dev/mem', 'rb') as f:
    m = mmap.mmap(f.fileno(), 4096, offset=0x3F801000, access=mmap.ACCESS_READ)
    d_regs = [
        (0x000,'CTRL'), (0x004,'STA'), (0x008,'ANA'), (0x010,'CLK'),
        (0x014,'CLT'), (0x018,'DAT0'),(0x01C,'DAT1'),(0x020,'DAT2'),
        (0x024,'DLT'), (0x028,'?28'), (0x400,'MISC')
    ]
    csi_regs = [
        (0x100,'ICTL'),(0x104,'ISTA'),(0x108,'IDI0'),(0x10C,'IPIPE'),
        (0x110,'IBSA0'),(0x114,'IBEA0'),(0x118,'IBLS'),(0x11C,'IBWP'),
        (0x120,'IHWIN'),(0x124,'IHSTA'),(0x128,'IVWIN')
    ]
    for off, name in d_regs + csi_regs:
        val = struct.unpack('<I', m[off:off+4])[0]
        print(f'[0x{off:03X}] {name:6s} = 0x{val:08X}')
"

# CM_CAM1 frecuencia real + power domain status
sudo python3 -c "
import mmap, struct
with open('/dev/mem', 'rb') as f:
    m = mmap.mmap(f.fileno(), 256, offset=0x3F101000, access=mmap.ACCESS_READ)
    for off, name in [(0x048,'CAM1CTL'),(0x04C,'CAM1DIV')]:
        val = struct.unpack('<I', m[off:off+4])[0]
        print(f'[0x{off:03X}] {name} = 0x{val:08X}')
"
```

**Resultado A1:** Ground truth obtenida vía `busybox devmem` (Python mmap bloqueado por CONFIG_STRICT_DEVMEM). Diferencias críticas encontradas: CLK/DAT=0x0005 (no 0x001D), 2-lane, CM_CAM1=100MHz, IBWP=0xCBD95000=0xC0000000|phys. Implementadas en V103.

---

### ~~V103~~ — Match Pi OS exactamente ← COMPLETADO (FALSIFICADO)

CLK=DAT=0x0005, 2-lane, CM_CAM1=100MHz, ICTL=0x00D80007, CLKGATE no escrito.
Hardware V103: todos los registros iguales a Pi OS confirmados. **STA=0 persiste.**
Conclusión: no es ningún registro de Unicam. La diferencia está en otro lugar.

---

### ~~V104~~ — Modo reloj MIPI 0x0310=0x00 ← COMPLETADO (FALSIFICADO)

k_imx708_init tenía `{0x0310, 0x01}` = continuous HS clock. Cambiado a `0x00`.
Hardware V104: `0x0310(clk_mode)=0` confirmado. **STA=0 persiste.**

---

### ~~V105~~ — IBSA0 bus addr 0xC0000000 ← COMPLETADO (FALSIFICADO)

IBSA0=0xC2240200 confirmado en hardware. STA=0 persiste. Bus address no era la causa.
Cambio conservado porque es correcto per Pi OS ground truth.

---

### ~~V106~~ — CLKGATE @0x3F802004 (CSI1) ← COMPLETADO (necesario pero no suficiente)

CLKGATE dirección corregida a 0x3F802004 (CSI1). Readback=0x15 confirma escritura exitosa.
STA=0 persiste — CLKGATE solo no era suficiente sin domain power.

---

### ~~V107~~ — CLKGATE write post-CPE ← COMPLETADO (FALSIFICADO)

Movido CLKGATE write de Step 0B (antes de CPR) a Step 14B (después de CPE), igualando
orden de Linux `unicam_start_rx()`: CPR → registros → CPE → clk_write() → MISC → LIP.
STA=0 persiste.

---

### ~~V108~~ — SET_DOMAIN_STATE + SET_CLOCK_RATE ← **COMPLETADO (ROOT CAUSE CONFIRMADO ✓)**

**ROOT CAUSE REAL.** Linux usa `SET_DOMAIN_STATE` (0x00038030, domain=14) vía raspberrypi-genpd.
Nuestro `SET_POWER_STATE` (0x00028001, device=0x0d) siempre devolvió state=0x02 — namespace incorrecto.

Resultado HW: **STA>0 por primera vez** (STA_accum=0xD001, ISTA=0x05, IBWP avanza, ruido en HDMI).
FEI no dispara porque CPR-post-stream diagnóstico destruye la detección de Frame End.

**⚠️ INCIDENTE:** `git checkout src/camera_unicam.cpp` revirtió a V93 durante pruebas del simulador.
Código V105–V108 regenerado manualmente desde contexto de conversación. `hardware_sim.h` sobrevivió.

---

### ~~V109~~ — Eliminar CPR-post-stream + fast-poll ← COMPLETADO ✓

HW: PI0 capturado consistentemente en fast-poll. Pipeline total ~1591ms (debayer 1100ms por DMA overwrite).

---

### ~~V110~~ — DMA stop/restart + RAW10 debayer (A5) ← COMPLETADO (parcial)

Cambios:
1. **stop_unicam_dma()**: CPE=0 tras PI0/FEI → congela buffer inmediatamente
2. **restart_unicam_dma()**: re-enable CPE + CLKGATE + MISC + LIP antes de cada captura
3. **DC CIVAC**: cache clean+invalidate del buffer DMA completo
4. **camera_debayer.cpp**: reescrito de RAW8 a RAW10 packed (5 bytes → 4 pixels)
5. **debayer_raw10_to_fb()**: 480×480 letterboxed en 640×480, RGGB → ARGB

HW resultado: RGB 29ms (de 1100ms). Orientación correcta. **Barras negras + monocromático persisten.**

---

### V111 — Diagnósticos hex dump ← LISTO PARA FLASH

Agrega después de capture + cache invalidation:
1. **IBWP print**: verifica `delta == FRAME_SZ (1658880)` — frame completo escrito
2. **Hex dump raw[0..19]** y **raw[row100,0..19]**: verifica formato RAW10-packed
3. **nonzero_sample**: 100 muestras diagonales — 0/100 = cache no funciona
4. **RGGB pixel decode** en (336,0) y (400,200): verifica separación de color

Hipótesis a falsificar:
- **DC CIVAC escribe zeros sobre datos DMA**: BSS-zeroed cache lines → CIVAC write-back → overwrites
- **Formato no es RAW10-packed**: sensor podría enviar RAW10 unpacked (2 bytes/pixel)
- **Fase Bayer incorrecta**: IMX708 binned puede no ser RGGB
- **Embedded data lines**: primeras líneas son metadata, no pixels

Sim PASS ✓. kernel8.img listo.

---

### ~~A2 — V97/V98: Sweep settle timing~~ ← COMPLETADO (FALSIFICADO)
All settle values 1–5 at 100MHz tested. STA=0 for all. Settle timing not the cause.

### ~~A3 — V97: IMX708 readback~~ ← COMPLETADO (CONFIRMADO CORRECTO)
fmt=0x0A0A ✓, bin_en=1 ✓, bin_type=0x22 ✓. Sensor configuration is correct.

### ~~A2c — V99: CM_CAM1=250MHz~~ ← COMPLETADO (FALSIFICADO)
CM_CAM1 at 250MHz also gives STA=0. Clock frequency not the cause.

---

### A5 — Post-STA: debayer RAW10 y loop cerrado

Una vez que `STA > 0` y `IBWP` avance más allá de `IBSA0`:

**Debayer RAW10** — `camera_debayer.cpp` asume RAW8. RAW10 packed = 4 pixels en 5 bytes:

```cpp
// Grupo de 5 bytes → 4 pixels de 10 bits
P0 = (byte[0] << 2) | ((byte[4] >> 0) & 0x3)
P1 = (byte[1] << 2) | ((byte[4] >> 2) & 0x3)
P2 = (byte[2] << 2) | ((byte[4] >> 4) & 0x3)
P3 = (byte[3] << 2) | ((byte[4] >> 6) & 0x3)
// Conversión directa a float32:
px_f32 = px_10bit * (1.0f / 1023.0f)
```

Implementar con `vld1q_u8` + shift/mask NEON. Verificar orientación del sensor en el primer
frame (no asumir — la conclusión sobre cruce D0/D1 cambió entre V17 y V35).

**Loop cerrado:**

```cpp
while (true) {
    watchdog_kick();                        // antes del blocking wait
    camera_capture_frame(g_input_tensor);   // bloquea hasta ISTA_FE (~18 ms)
    run_yolo_complete(g_input_tensor);
    video_render_detections();
    flush_to_ram();
}
```

---

## Track B — Optimizaciones de performance *(paralelo al Track A)*

### B1 — INT8 post-training quantization ← mayor impacto

**Ganancia estimada:** −300 ms → total ~210 ms / ~4.7 FPS | **Esfuerzo:** Alto

Host (PyTorch): calibrar con 100–200 imágenes COCO, exportar escalas por capa. Bare-metal:
reemplazar `float32x4_t` por `int8x16_t` con `vmull_s8` + `vpaddlq_s16`; dequantize solo
en salidas de bloque C3/SPPF. Beneficio secundario: weights 7.5 MB → ~1.9 MB.

### B2 — conv1x1 8-ch

**Ganancia estimada:** ~10 ms | **Esfuerzo:** Bajo

Mismo patrón que `conv2d_partial_8ch()` para K=3. Aplica a detection-head 1×1 layers con
`C_out ≥ 32 && C_out % 8 == 0`. Cambio en `ops.cpp` + layout en `export_model.py`.

### B3 — PRFM prefetch en weight loads

**Ganancia estimada:** 5–15% | **Esfuerzo:** Medio

`__builtin_prefetch(&w[(pos+3)*8], 0, 1)` en el inner loop de `conv2d_partial_8ch()`.
La brecha entre speedup teórico 2× y medido 1.22× indica bottleneck en bandwidth LPDDR2.

### B4 — L0 stem K=6 unroll especializado

**Ganancia estimada:** ~5–10 ms | **Esfuerzo:** Bajo

`C_in=3` fijo permite unroll completo del loop `ci`. Crear especialización
`template<> void conv2d_partial<3, 6>(...)`.

---

## Deuda técnica

| ID | Archivo | Descripción | Prioridad |
|:---|:--------|:------------|:----------|
| ~~T1~~ | ~~`hardware_sim.h:59-64`~~ | ~~Comentarios BIT(3)/BIT(4) incorrectos~~ | ~~Alta~~ — **RESUELTO V108**: comentarios actualizados |
| ~~T2~~ | ~~`hardware_sim.h`~~ | ~~CLKGATE dirección~~ | ~~Media~~ — **RESUELTO V108**: `0x3F802004` correcto (CSI1) |
| T3 | `camera_unicam.cpp` | `FRAME_W` hardcodeado como RAW8; parametrizar por data type | Media |
| ~~T4~~ | ~~`imx708_regs.h`~~ | ~~`0x0901` binning type readback 0x34 vs esperado 0x22~~ | ~~Baja~~ — **RESUELTO V97**: bin_type=0x22 ✓ confirmado en hardware |

---

## Métricas objetivo

| Métrica | Actual | Sin INT8 + cámara | Con INT8 + cámara |
|:--------|:------:|:-----------------:|:-----------------:|
| FPS inferencia | ~2 FPS | ~2 FPS | ~4.7 FPS |
| Latencia | ~511 ms | ~511 ms | ~215 ms |
| Fuente de imagen | `test_image.bin` | Camera v3 live | Camera v3 live |

---

## Regla de proceso

> **Toda versión nueva se valida en QEMU sim antes de flashear.**
> Un FAIL en QEMU garantiza fallo en hardware.
> Un PASS en QEMU no garantiza éxito, pero elimina bugs de secuencia de registros.
