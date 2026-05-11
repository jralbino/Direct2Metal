# Direct2Metal — Plan de Desarrollo

> Última actualización: 2026-05-10
> Estado: **V164** — YOLO re-activado sobre el pipeline V163. Detecciones COCO funcionando (c=0 person, c=40 wine glass, ~65% conf). **~1.9 fps** (518 ms/frame, inferencia FP32 domina). Cámara a calidad libcamera.

---

## Resumen ejecutivo

Motor de inferencia (Phases 1–8): YOLOv5n 320×320, **~526 ms / ~2 FPS**, 4 cores A53, NEON SIMD, MMU+cache, ISO 26262.

Phase 9 desbloqueada en V108 (`SET_DOMAIN_STATE` era el root cause). Frames llegando desde V109.
V118 base: V111 (hex dump diagnostics) + BGGR Bayer fix + FS-to-FS capture + brightness stretch.
Barras horizontales y shift izquierda-derecha persisten — causa no es captura ni cache.

---

## Cronología de hitos

| Versión | Hito |
|:--------|:-----|
| V84–V107 | 18 escenarios SW agotados, STA=0 en todos |
| **V108** | **BREAKTHROUGH: SET_DOMAIN_STATE → STA=0xD001, DMA activo** |
| V109 | CPR-post-stream eliminado, PI0 consistente, 526ms |
| V110 | DMA stop/restart + debayer RAW10 completo, 29ms RGB |
| V111 | Diagnóstico: frame corto 704/864, sensor al black level |
| V112 | Test pattern color bars → data path confirmado |
| V113 | 16× gain activo, test_pattern=0; BGGR color bug visible |
| V114 | IBWP polling → 864/864 líneas consistente, YOLO detecta objetos reales |
| V115 | LIP-after-FS: elimina barras horizontales del frame anterior |
| V116 | BGGR Bayer fix (R↔B corregido), watchdog 8s+3 kicks, SD card save |
| V117 | Sim BGGR fix, tech debt T1/T2/T3 resueltos |
| V118 | Base V111 + BGGR + FS-to-FS capture + brightness stretch |
| V119–V162 | (Subproyecto `camara_direct/`) — debug del lane-swap, ISP libcamera, multi-core debayer. Ver `camara_direct/PROGRESS.md`. |
| V163 | Back-port V162 al padre: BGGR flip 0x0101=0x03, HS clock continuo 0x0310=0x01, LSC LUT, ISP libcamera, lanes runtime-correctos, ping-pong DMA, multi-core debayer async, YOLO desactivado → 52 fps cámara-sólo. |
| **V164** | **YOLO re-activado sobre V163. `run_yolo_complete` usa `parallel_debayer_start/wait` para el render FB. Detecciones COCO confirmadas en HW (person 66%, wine glass 65%). ~1.9 fps, inferencia FP32 domina.** |

---

## Estado actual — V164 (YOLO + pipeline V163)

### Resueltos en V164
- ✅ **YOLO end-to-end funcionando**: `[DET] c=0 %=66` (person) y `[DET] c=40 %=65` (wine glass) confirmados sobre escena real. Confirma que el ISP V162 entrega el input correcto al modelo.
- ✅ **FB render multi-core en modo YOLO**: `parallel_debayer_start/wait` reemplaza al `camera_render_fullres` single-core (~21 ms → ~7 ms). El render no domina; YOLO sí.
- ✅ **Bboxes coherentes con frame mostrado**: un solo `video_flush()` al final del iter (después de overlay) elimina el flash sin boxes.
- ✅ **Eliminado `run_camera_only`** (modo prueba V163). Si necesario para diagnóstico, revivir desde commit `c3461a4`.

### Métricas HW (2026-05-10)
```
[F41] [DET] c=0  %=66   → person
[T] 518ms        → 1.93 fps
[F58] [DET] c=40 %=65   → wine glass
[T] 520ms        → 1.92 fps
```

### Cuello de botella actual
Inferencia FP32 con NEON 4-wide sobre 4 cores ≈ 500 ms / frame. Cámara (~25 ms) y render (~7 ms) son ruido al lado. Optimización en Track B.

---

## Estado V163 — pipeline cámara (sin YOLO)

### Resueltos en V163
- ✅ **Lane swap**: `DAT1=0x06000005` (clock-pattern, no data) + `0x0310=0x01` (HS continuo). Escenas reales sin bytes mezclados.
- ✅ **ISP libcamera byte-exact**: BLC=16 (MSB8) → WB Q8 (535,256,455) → CCM Q10 4640K → gamma sRGB LUT 256 entradas (51 puntos `rpi.contrast`). Reemplaza el `(v-16)*4/*2/*4` AWB heurístico.
- ✅ **Lens-shading correction**: 108-reg LUT (`k_imx708_lsc[]`) entre common e init. Esquinas dejan de oscurecerse.
- ✅ **Bayer BGGR** post-flip H+V 0x0101=0x03 — color natural, sin piel azul.
- ✅ **CMP0 = 0x80000301** (libcamera runtime). El V134 disable era erróneo.
- ✅ **Continuous DMA ping-pong** (V158 pattern): dos buffers `g_raw_a/b`, LIP por iter, CPE nunca cae. FSI wait ≈ 19 ms estricto.
- ✅ **Multi-core debayer async**: nuevo `TASK_DEBAYER`. Cores 1-3 procesan bandas de 120 rows del FB en paralelo con el FSI wait del core 0. `parallel_debayer_start/wait`.
- ✅ **AE on CIT**: P-controller con group-hold I2C, slew ±32 líneas, periodo 4 frames.

### Métricas HW (2026-05-10, YOLO disabled)
```
[CAM] cap=19 sync+flush=0 disp=0 total=19 fps=52
```
Supera al subproyecto (`wait=21 fps=47.61`) porque el padre debayera 640×480 en lugar de 1920×1080.

### Eliminado (legacy/workaround/diag)
- ❌ `deshift_halfline_pass` + `DeshiftState` (clasificador 4-state). Estaba reparando un bug de transporte que ya no existe; estaba *dañando* frames buenos.
- ❌ `diag_dump_frame` + `DIAG136/144` por-row dumps.
- ❌ `debayer_diag_*`, `debayer_split_view_fb`, `debayer_test_pattern_fb`.
- ❌ `dump_unicam_regs`, `stop_unicam_dma`, `restart_unicam_dma`, `invalidate_frame_dcache`.

### Pendiente
- ⬜ **YOLO re-integration** (siguiente sesión). YOLO usa cores 1-3 para conv2d/conv1x1 — colisión con el debayer async actual. Plan en `memory/camara_direct_parent_port_done.md`.

---

## Track A — YOLO re-enable (siguiente paso)

1. Re-rutear `kernel_main` a `run_yolo_complete()` (o un híbrido).
2. Decidir cómo compartir cores 1-3 entre debayer y conv2d:
   - **(a)** Secuencial: debayer FB → sync → YOLO inference → bbox overlay. Simple. fps colapsa a la inferencia (~2 fps con YOLOv5n NEON).
   - **(b)** YOLO posee cores 1-3, FB se dibuja directo desde el chw320 (calidad menor) o se omite el render full-res.
   - **(c)** Interleave: debayer FB durante un período del sensor, YOLO durante el siguiente. Complejo.
3. Primera versión: opción (a). La inferencia domina el budget — el throughput del pipeline cámara importa solo para tener input fresco a YOLO.
4. Overlay de bboxes con `draw_rect` debe ser DESPUÉS de `parallel_debayer_wait()` para no ser sobrescrito.

---

## Track B — Performance YOLO (siguiente foco tras V164)

Cuello identificado: inferencia FP32 ~500 ms. Plan en orden ganancia/esfuerzo:

| # | Fase | Esfuerzo | fps acumulado | Notas |
|:--|:-----|:--------:|:--------------|:------|
| B0 | **Profilar por capa** | 1 h | (sin cambio) | UART por etapa: `t_l0`, `t_backbone`, `t_neck`, `t_nms` ya existen, sólo falta imprimirlos. |
| B1 | **Resolución 320 → 192** | 1–2 d | **~5** | Cambia `CROP_SZ`, `STEP_Q8`, `OUT_W/H`, anchors / grids. YOLOv5n entrenado a 640 tolera 192. mAP cae ~20%. |
| B2 | **NEON 8-ch + PRFM** | 3–4 d | **~6** | Extender `conv2d_partial_8ch` a `conv1x1`, `prfm pldl1keep` para weights próximos. |
| B3 | **INT8 quantization** | 1–2 sem | **~13–15** | Calibrar offline con ~50 frames reales, kernels con `vmlal_s16` + `vmovl_s8`. A53 no tiene SDOT. Memoria 4× menos → cache hits. |
| B4 | **Frame-skip detección** | 0.5 d | **~25 percibido** | Inferir cada N frames, mantener bboxes. Cámara sigue a 52 fps. |
| B5 | **Winograd F(2×2, 3×3)** | 1 sem | **+30% sobre 3×3** | Sólo si profiling muestra que convs 3×3 dominan tras INT8. |
| B6 | **Async cámara + YOLO** | 1 d | **+3–5%** | Sólo vale la pena después de B3 cuando YOLO baje a ~50 ms. |

**Stop realista** con A53 bare-metal: ~15 fps reales / 25 percibidos tras B1+B2+B3+B4. Para más: Pi 4/5 (A72/A76 con SDOT) o modelo más pequeño (NanoDet, PicoDet).

---

## Métricas

| Métrica | V118 (camera bug) | V163 (camera-only, YOLO off) |
|:--------|:------------------|:------------------------------|
| FPS | ~1.7 FPS (592ms/frame) | **52 FPS (19 ms/frame)** |
| Capture | FS-to-FS DMA-stop, delta=0 | Continuous DMA ping-pong, 2-FSI wait |
| Debayer FB | ~92 ms single-core | ~7 ms en 3 cores paralelo (escondido en FSI wait) |
| Frame | 864/864 líneas | 864/864 líneas |
| Bayer | BGGR (correcto) | BGGR + ISP libcamera-exact |
| Color | Verde dominante (sin AWB) | WB+CCM+gamma libcamera, AE en CIT activo |
| Barras / lane-swap | **SÍ** | ✅ Resueltas (lanes runtime-correctos, HS clock continuo) |
| YOLO | Single-buffer + fp32 conv NEON | **Desactivado** — siguiente paso |

---

## Deuda técnica

| ID | Archivo | Descripción | Prioridad |
|:---|:--------|:------------|:----------|
| ~~T1~~ | ~~`hardware_sim.h:59-64`~~ | ~~Comentarios BIT(3)/BIT(4) de V90~~ | **RESUELTO V117** ✓ |
| ~~T2~~ | ~~`hardware_sim.h`~~ | ~~`clkgate_enabled` dice dirección incorrecta~~ | **RESUELTO V106** ✓ |
| ~~T3~~ | ~~`camera_unicam.cpp`~~ | ~~`FRAME_W` hardcodeado RAW8~~ | **RESUELTO V91** ✓ |
| T5 | `camera_debug.md` | "Confirmed Hardware Facts" tiene entradas invalidadas | Baja |
| ~~T4~~ | ~~`imx708_regs.h`~~ | ~~bin_type 0x34 vs 0x22~~ | **RESUELTO V97** ✓ |
| T6 | `camera_debayer.cpp` | Barras horizontales + shift en FB render | **Alta** |

---

> **Regla de proceso**: validar en QEMU sim antes de flashear.
> **Lección V108**: commit antes de `git checkout` en archivos modificados.
> **Lección V118**: no limpiar 1.6MB buffer byte-a-byte (watchdog kill). Boot fill color no afecta barras.
