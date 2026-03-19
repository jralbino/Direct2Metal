# Direct2Metal — Plan de Desarrollo

> Última actualización: 2026-03-18
> Estado: **V118** — BGGR fix + FS-to-FS capture + brightness stretch. Barras y shift pendientes.

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
| **V118** | **Base V111 + BGGR + FS-to-FS capture + brightness stretch** |

---

## Estado actual — V118

### Resueltos
- ✅ **BGGR Bayer fix**: R↔B corregido en 3 debayer paths (NEON, scalar, FB)
- ✅ **FS-to-FS capture**: frame completo 864/864 líneas (delta=0 confirmado)
- ✅ **Brightness stretch**: resta black level 16, ganancia 4× en FB path
- ✅ **Boot fill negro**: elimina residuo de green fill previo
- ✅ **Video flush inmediato**: flush FB después de render, antes de YOLO overlay

### Pendientes — Barras horizontales y shift
- ⬜ **Barras horizontales verdes**: persisten con boot negro + flush inmediato + FS-to-FS. NO son de cache, boot fill, ni captura incompleta. Causa desconocida — posible stride mismatch o framebuffer pixel format issue.
- ⬜ **Shift izquierda-derecha**: imagen partida verticalmente en el centro. Misma causa que barras. Persiste con LIP-after-FS y FS-to-FS.
- ⬜ **Verde dominante**: sensor G > R,B (Gr/Gb=29-31 vs R=22,B=23). Necesita AWB estático.
- ⬜ **FEI never fires**: aún inexplicado pero no bloqueante.

### Descartados como causa
- ❌ Boot fill verde (cambiado a negro, sin efecto)
- ❌ Cache coherency / flush timing (video_flush inmediato, sin efecto)
- ❌ PI0 frame corto (FS-to-FS da delta=0, sin efecto en barras)
- ❌ LIP timing (LIP-after-FS probado, sin efecto en barras)
- ❌ Buffer clear (probado, causó watchdog reset por lentitud)

---

## Track A — Cámara: próximos pasos

### Investigar barras horizontales
1. Renderizar test pattern directo al FB (sin cámara) → confirmar si FB format es correcto
2. Verificar que `debayer_raw10_to_fb` escribe TODOS los pixels correctamente
3. Probar mapear framebuffer como Device memory (sin cache) → lento pero elimina coherency
4. Comparar output del debayer con datos RAW conocidos (test pattern sensor)

### AWB (white balance)
- Gains estáticos: R×1.8, B×1.5 (típico IMX708 daylight)
- Aplicar en debayer FB path después de brightness stretch

---

## Track B — Performance *(paralelo)*

| # | Optimización | Ganancia estimada | Esfuerzo |
|:--|:-------------|:-----------------|:---------|
| B1 | INT8 quantization | −300ms → ~4.7 FPS | Alto |
| B2 | conv1x1 8-ch | ~10ms | Bajo |
| B3 | PRFM prefetch | 5–15% | Medio |
| B4 | L0 stem K=6 unroll | ~5–10ms | Bajo |

---

## Métricas

| Métrica | V118 (actual) |
|:--------|:-------------|
| FPS | ~1.7 FPS (592ms/frame) |
| Capture | FS-to-FS, delta=0 |
| Debayer FB | ~92ms (post-cache) |
| Frame | 864/864 líneas |
| Bayer | BGGR (correcto) |
| Color | Verde dominante (sin AWB) |
| Barras | **SÍ — causa desconocida** |

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
