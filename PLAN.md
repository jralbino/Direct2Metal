# Direct2Metal — Plan de Desarrollo

> Última actualización: 2026-03-18
> Estado: **V116** — BGGR fix + watchdog fix + SD card frame save. Loop cerrado funcional.

---

## Resumen ejecutivo

Motor de inferencia (Phases 1–8): YOLOv5n 320×320, **~526 ms / ~2 FPS**, 4 cores A53, NEON SIMD, MMU+cache, ISO 26262.

Phase 9 desbloqueada en V108 (`SET_DOMAIN_STATE` era el root cause). Frames llegando desde V109.
Pipeline completamente funcional desde V114 (IBWP polling, 864/864 líneas).
V115 elimina barras del frame anterior con LIP-after-FS.
V116 corrige color verde dominante (BGGR), watchdog resets, y agrega guardado en SD.

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
| **V116** | **BGGR Bayer fix (R↔B corregido), watchdog 8s+3 kicks, SD card save** |

---

## Estado actual — V116

### Resueltos
- ✅ **Frame corto**: IBWP polling → `wrap, 0 lines overwritten` consistente (V114)
- ✅ **Imagen oscura**: gain 16× + test pattern confirmaron data path (V112)
- ✅ **Barras horizontales**: LIP-after-FS resetea IBWP a IBSA0 antes de captura (V115)
- ✅ **Color verde dominante**: BGGR Bayer fix en todos los debayer paths (V116)
- ✅ **Watchdog resets**: timeout 4s→8s + 3 kicks adicionales en backbone/neck (V116)
- ✅ **SD card frame save**: sdcard.cpp — EMMC + FAT32 + PPM, sin buffer grande (V116)

### Pendientes
- ⬜ **AWB (auto white balance)**: sin AWB, imagen tendrá dominancia de color leve por las curvas del sensor. Fix futuro: gains R/G/B estáticos calibrados.
- ⬜ **FEI never fires**: aún inexplicado pero no bloqueante (IBWP polling funciona sin FEI).

---

## Track A — Cámara: verificar en HW

### A1 — Flujo en V116

```
boot → sdcard_init() → capture_frame (warm-up) → sdcard_save_ppm() → YOLO loop
```

SD card: FRAME.PPM en partición boot FAT32. Leer desde Linux:
```bash
cp /boot/firmware/FRAME.PPM /tmp/ && eog /tmp/FRAME.PPM
```

### A2 — Loop cerrado (activo desde V114)

```cpp
while (true) {
    watchdog_kick();
    camera_capture_frame(g_input_tensor);  // IBWP polling, DMA stop after wrap
    run_yolo_complete();
    video_render_detections();
}
```

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

| Métrica | V116 (actual) | Con INT8 |
|:--------|:-------------|:---------|
| FPS | ~2 FPS | ~4.7 FPS |
| Latencia total | ~526 ms | ~230 ms |
| Debayer | 29 ms | 29 ms |
| Frame correcto | Sí (864/864 líneas, BGGR OK) | — |
| Color | Correcto (sin AWB) | — |

---

## Deuda técnica

| ID | Archivo | Descripción | Prioridad |
|:---|:--------|:------------|:----------|
| T1 | `hardware_sim.h:59-64` | Comentarios BIT(3)/BIT(4) de V90 incorrectos | Alta |
| T2 | `hardware_sim.h` | `clkgate_enabled` dice `0x3F802004` → `0x3F802000` | Media |
| T3 | `camera_unicam.cpp` | `FRAME_W` hardcodeado RAW8 | Media |
| T5 | `camera_debug.md` | "Confirmed Hardware Facts" tiene entradas invalidadas | Baja |
| ~~T4~~ | ~~`imx708_regs.h`~~ | ~~bin_type 0x34 vs 0x22~~ | **RESUELTO V97** ✓ |

---

> **Regla de proceso**: validar en QEMU sim antes de flashear.
> **Lección V108**: commit antes de `git checkout` en archivos modificados.
