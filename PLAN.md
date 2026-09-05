# Direct2Metal — Plan de Desarrollo

> Última actualización: 2026-09-04
> Estado: **V194** — instrumentación `[SOC]` (reloj real + throttle + temperatura por mailbox). Línea base HW **con condiciones registradas: 504 ms @ 1000 MHz, `thr=0x0`, 43-51 °C**, reproducible a ±0 ms en 3 corridas. El 492 ms de V192 se midió sin registrar condiciones y no se reprodujo; **no es throttling térmico** (falsado con medición directa, ver fila V194). V193 — **Winograd F(2×2,3×3) implementado, validado y CERRADO** (verdicto negativo, ver fila V193). Geometría vigente desde **V192**, campo de visión completo (entrada no cuadrada 320×192). YOLOv8n es fully-conv → mismos pesos. Golden = frame de cámara recapturado (`clock` 56 %, huella `[ABS]` PASS 16/16). En GitHub `main` (hasta `3ad18eb`; V193 aún sin push).
>
> **SESIÓN 2026-09-04: 799 → 492 ms (−38 %) + FoV completo.** V187 relojes stock 1 GHz + `FRAME_SKIP_N=4`; V188 AE altas-luces + AWB; V189 golden real; V190 conv1x1 transpose (fin del thrash de sets L1, −17 %); V191 P8 por-posición 8ch+4ch (−18 %); V192 FoV completo (−9 %); V193 Winograd — implementado y **descartado** (más lento, ver abajo).
>
> **CONTINUAR — A/B de modelos.** El plan anterior asumía `app/yolo_v5n_coco/` ya portado al layout `bsp/runtime/app` — **no existe**; lo que hay es el v5n viejo en el layout plano `src/` previo al refactor V171 (commit `a842b62`, V168 de este mismo repo). Portarlo es reescritura real (exporter, grafo, decode anchor-based vs DFL), no una copia de carpeta — várselo esfuerzo de sesión completa antes de retomarlo. Hipótesis sin cambiar: v5n (½ FLOPs) detecta el reloj mejor que el 56 % del v8n, o permite subir a 384×224 a igual latencia. Ver GOALS.md §G2.
> Levers descartados (medidos): INT8 (sin SDOT), interleave FMLA, store contiguo, **Winograd F(2×2,3×3) (V193 — más lento, no más rápido)**. `tools/env` roto (venv de 0 bytes — recrear para los exporters torch).
> V191 — P8 por-posición (541 ms). V190 — conv1x1 transpose (664). V189 — 1 GHz (799).
> V188 — cámara: AE con protección de altas luces (blancos quemados resueltos: recorte R/B 42/45 % → 4/6 %), AWB grey-world, imagen de cámara en pantalla (`SHOW_CAMERA`), `FRAME_SKIP_N=4` por defecto, relojes stock 1 GHz (V187), captura de frame real por UART (`make capture`, V185). Ver filas V185–V188.
> Estado previo — **V184**: `test_image.bin` regenerado a 256² (era un fósil de 320² desde V169 → planos de color desalineados → `[DET] none`). Ahora el golden determinista **detecta**: person 88 / bus 85 / person 77 / person 71 sobre `bus.jpg`. V183: banco HW desatendido (`make bench`), P8 (−6.1% A/B), profiler por capa; 1276 ms @600 MHz, heads 34%. Tier 2 INT8 cerrado (GOALS.md). `tools/env` roto (intérpretes de 0 bytes).
> (Estado previo: **V165** — B0 profiling UART + B1 192×192 + B2 NEON 8-ch conv1x1 + PRFM.)

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
| V164 | YOLO re-activado sobre V163. `run_yolo_complete` usa `parallel_debayer_start/wait` para el render FB. Detecciones COCO confirmadas en HW (person 66%, wine glass 65%). ~1.9 fps, inferencia FP32 domina. |
| **V165** | **B0: per-layer profiling UART `[P] cap= l0= bb= neck= head= render=`. B1: inference 320→192 (parametrizado por `YOLO_IN` macro, todas las dimensiones espaciales derivadas). B2: 8-output-channel hot path en `ops_neon_conv1x1_kernel` + `__builtin_prefetch(pldl1keep)` en conv1x1 y conv2d_partial_8ch. B3 plan documentado en `tools/B3_INT8_PLAN.md`.** |
| V166–V182 | (Sin fila aquí; ver commits `git log` V175–V179 INT8 Tier 1/2 + B4 frame-skip, y GOALS.md §G2 para el veredicto. V180 ping-pong no bloqueante, V181 `DEBUG=` thumbnail, V182 tracker con predicción de velocidad — en árbol de trabajo.) |
| **V183** | **Banco HW desatendido** (`make bench`, `tools/HWBENCH.md`): stub `raspbootin64` en la SD + kernel `-DSERIAL_BOOT` de 80 KB por UART + `d2m_data.bin` (fp32 + test_image) cargado por el VPU vía `initramfs` a `0x08000000` + reset por RTS→RUN. Golden = detecciones + huella `[ABS]` de 16 checkpoints (QEMU ≡ HW a la milésima). **P8 weight-stationary** en `conv2d_partial_8ch` (K=3/s1/p1, tiles interiores, ci externo, pesos leídos 1× por tile): **A/B en HW 1359 → 1276 ms (−6.1%)**; P3 head 320→260, L4 96→82, L15 119→112; L2 sin cambio (kernel 4ch). **Profiler por capa** (`prof_*` en bsp, marks en el grafo + `C2F_MARK` en L6): heads 431 ms (34%; P3 260), L2 C2f @S4 154, L15 112, bb 502 / neck 289. Código muerto fuera (`runtime/conv2d.cpp`, `runtime/neon/*.s`, 10 declaraciones fantasma en `ops.h`). `CLAUDE.md` nuevo. Kill-switch `-DD2M_NO_P8` para A/B. Próximos levers: P8 en el kernel 4ch (L2), Winograd F(2×2,3×3); INT8 descartado (Tier 2). |
| **V184** | **Fix del fósil `test_image.bin`.** Desde V169 (paso a 256²) el tensor seguía siendo 3×320×320 (1 228 800 B): el kernel lee los primeros 3·N² floats, así que R/G/B quedaban desalineados y el grafo veía rayas → `[DET] none` con `P3_CLS amax≈18–25` (logits enormes, cajas inválidas). `tools/convert_image.py` reescrito: lee `YOLO_IN` de `bsp/bsp.h`, salida a `app/<APP>/`, Pillow+numpy (la venv `tools/env` tiene intérpretes de 0 bytes). `test_image.bin` → 786 432 B desde `bus.jpg`. Resultado QEMU (fp32, `SERIAL_BOOT`): **person 88, bus 85, person 77, person 71** — los mismos objetos que el v5n@320 de D2M (88/85/82/74). `GOLDEN`/`GOLDEN_ABS` regenerados; `d2m_data.bin` 13 394 496 B (recopiar a la SD). Nada del grafo cambió. **Confirmado en HW 2026-09-04: detections PASS + fingerprint PASS 16/16, 1284 ms.** |
| **V185** | **Captura de frame real de cámara → golden** (`make capture`, `tools/HWBENCH.md`). Imagen serial aparte `kernel8_capture.img` (`-DCAPTURE_FRAME=N`, objetos `cap_*.o`) con la cámara **encendida**; en el frame N (AE asentado) `run_yolo_complete` vuelca `cam_frame` —el tensor exacto que entra al modelo tras debayer+ISP— por UART en base64 entre `[CAP] begin len= crc=` y `[CAP] end` (`watchdog_kick` cada 32 líneas; ~90 s a 115200), y sigue infiriendo en vivo. `hwbench.py --capture` reensambla + CRC32; `tools/capture_to_test_image.py` inspecciona (stats por canal, preview PNG) e instala como `test_image.bin`. Sin driver SD (`bsp/sdcard.cpp` sigue sin linkar) y sin pasos físicos salvo recopiar el blob al final. Primera captura HW 2026-09-04: 786 432 B, CRC OK, `[DET] c=0 %=67` en vivo sobre la misma escena. Instalación como golden pendiente de que el usuario valide la escena. |
| **V186** | **`SHOW_CAMERA ?= 1`**: el canvas vuelve a ser el frame de cámara (`camera_render_fullres`, 640×360 letterbox en filas 60..419, misma geometría `CAM_DISP_*` de las cajas) en vez del negro de V167. HW: `render` 1 → 22 ms, inferencia sin cambio, person 92–96 % en vivo. `SHOW_CAMERA=0` restaura el canvas negro. `make capture CAPFILE=…`. |
| **V187** | **Relojes stock** (`arm_freq=1000 core_freq=400 sdram_freq=450`, sin `over_voltage=-2`) en `config.txt` y `config.serial.txt` — el perfil 600 MHz era por el rayo de subvoltaje alimentando desde el PC; ahora fuente de pared. **`FRAME_SKIP_N ?= 4`** por defecto (cámara+HUD a ritmo de captura, inferencia 1 de 4); bench y capture fuerzan N=1 vía `SERFLAGS -U/-D` porque `hwbench.py` necesita `[P]`/`[ABS]` en el frame que parsea. Timing HW a 1 GHz pendiente de la primera corrida tras copiar el `config.txt` a la SD. |
| **V188** | **Blancos quemados: era exposición, no balance.** Síntoma: R/B al 1.0 en 5–45 % de píxeles con G "intacto". Diagnóstico con `make capture` + estadística por canal: (1) AWB grey-world movió las ganancias <5 % (el WB del tuning era correcto para la sala); (2) la rejilla del AE mostraba ~la mitad del sensor ≥ 250 raw; (3) G *sí* saturaba — raw 255 − BLC 16 = 239 → gamma → 247/255 = 0.97, justo bajo un umbral 0.98 (artefacto de métrica). Causa: el AE (`AE_TARGET=100` sobre la media raw) sin protección de altas luces deja quemar escenas de alto contraste. **Fix en `camera.cpp ae_step()`**: `AE_SAT_LEVEL=250`, `AE_SAT_MAX_PCT=2`, término −16 −160·sat/1024 líneas fuera del clamp ±32; traza `[AE] cit= mean= sat= d=` (cada actualización en el build de captura, cada 32 en HW). HW: CIT 1131 → 420, `sat` 351 → 14/1024, recorte R/B 42/45 % → **3.9/6.5 %**, sin oscilación. **AWB grey-world** (`debayer_awb_update`, 32×32 bloques, excluye ≥250, IIR 1/4, clamp 0.5–3×, `make AWB=0` congela) se mantiene: inofensivo y protege ante cambio de luz; `[AWB] r= b=` cada 32 frames. Probé quitar el recorte post-WB en `isp_pixel`: **peor** (píxeles saturados en raw se vuelven más rojos) — restaurado. `CAPTURE ?= 90` (el build de captura actualiza AE cada ~5 s). |
| **V189** | **Golden sobre frame real + primer timing a reloj stock.** `test_image.bin` = frame 90 de la captura V188 (reloj de pared, AE/AWB convergidos, recorte R/B 3.9/6.5 %), instalado con `capture_to_test_image.py`; blob 13 394 496 B md5 `ca135c74…`. `make bench --print-golden` en HW a **1 GHz**: `GOLDEN=[(74,83)]` (clock), `GOLDEN_ABS` 16 checkpoints. **TOTAL 799 ms** (`cap=1 l0=13 bb=316 neck=186 head=262 render=18`) — exactamente ×1.6 sobre los 1276–1301 ms a 600 MHz: el grafo es compute-bound y escala lineal con el reloj. Por capa a 1 GHz: P3 head 158, L2 C2f @S4 104, L15 79, P4 head 73, L4 50, L6 (4×bot 3×3) 43. El bench determinista verifica ahora YOLO sobre un frame real del sensor tras debayer+ISP, y ese tensor es la entrada correcta para cualquier calibración futura. |
| **V190** | **conv1x1: transpose de la entrada por p-tile — arregla el thrash de sets de L1.** `ops_neon_conv1x1_kernel` hacía `vld1q_f32(in + ci*HW + p)` para ci consecutivos; con `HW=S4²=4096` (múltiplo de 2048) todos los ci caen en el **mismo set** de la L1 (32 KB / 4-way) → `C_in−4` conflict misses por grupo de salida. Desglose de L2 con `make C2F_PROF=2`: **cv2 1×1 (48→32 @64²) = 55 ms**, ~18× su piso de cómputo. Fix: transponer cada grupo de 4 posiciones de la entrada a `xt[C_in][4]` (contiguo en ci, ~residente en L1) **una vez**, y que todos los grupos de salida lo lean secuencial → lecturas strided de `(C_out/4)·C_in` a `C_in` por grupo. Se mantiene el cuerpo de 8 canales de salida (B2). Nuevo knob de perfilado `make C2F_PROF=N`. **HW 1 GHz: 799 → 664 ms (−17 %).** L2 cv2 55→7.8, cv1 15→3.8; L15 79→31; L12 35→31; L18 30→27. Bit-exacto (16/16 `[ABS]` + `[DET]` sin cambio). `conv1x1_slow` escalar como fallback para `C_in > 512` (nunca en este modelo; SPPF cv2 = 512 es el máximo). Perfil V190: **P3 head 152 ms (23 %)** ahora el ítem mayor — coste de diseño del head DFL (2×3×3 apilados × box/cls, a S8); L6 C2f 49, L4 42. |
| **V191** | **P8 por-posición-safe** en `conv2d_partial_8ch` **y** `conv2d_partial` (4ch, que no tenía P8). El P8 de V183 se activaba a granularidad de tile completo (`y_tile≥1 && y_end≤H_out-1 && …`); a S8=32 con TILE_W=8 solo hay 4 x-tiles y 8 y-tiles, así que **~60 % de las posiciones** caían en el camino lento por-posición que re-lee el bloque de 288 B de pesos por cada píxel (desglose con `make HEAD_PROF=1`: los 4 conv3×3 del P3 head = 141 ms). Ahora el fast path procesa la sub-caja "safe" (sin padding) **de cada tile** — `[max(y_tile,1), min(y_end,H_out-1)) × [max(x_tile,1), min(x_end,W_out-1))` — y solo el anillo de 1 px de padding usa el camino genérico. **HW 1 GHz: 664 → 541 ms (−18 %).** P3 head 3×3 141→105 (box0/box1/cls0 33→24.5, cls1 42→32); L2 bot 3×3 16→7.2 cada uno (el 4ch); L4 42→33; L12 31→24, L15 27, L18 27→20, L21 22→19. Bit-exacto (16/16 `[ABS]` + `[DET]` sin cambio en QEMU). Knob `make HEAD_PROF=1` (breakdown por-conv del P3 head). **Sesión V189→V191: 799 → 541 ms (−32 %), 2 fixes de eficiencia de kernel, cero cambio numérico.** Perfil V191: **head 3×3 105 ms** el ítem mayor — a ~3.5× del piso de cómputo del A53 (mismo veredicto que D2M: cerca del límite de la mezcla de instrucciones). Siguiente lever = Winograd F(2×2,3×3). |
| **V192** | **Campo de visión completo — entrada no cuadrada.** El debayer hacía un recorte central 864×864 del sensor 1536×864 (solo el 56 % del ancho — el "zoom" que notó el usuario y ceguera lateral). Ahora el sensor entero → tensor **320×192** (`YOLO_W`/`YOLO_H` en `bsp.h`, ambos /32): resize isótropo 1536×864→320×180 + letterbox de 6 px arriba/abajo. YOLOv8n es fully-conv → **mismos pesos**, el decode ya toma `gh`/`gw` por separado. Cambios: `bsp.h` (YOLO_IN → YOLO_W/H + LB consts), `yolo_v8n.cpp` (YOLO_S* → par H/W, ~130 sitios mecánicos; L0 conv, cam_frame, dump de captura `w=/h=`, escala de bbox del display), `camera_debayer.cpp` (`debayer_raw10_to_chw_yolo` full-FoV), `tools/{convert_image,capture_to_test_image,hwbench}.py` (leen YOLO_W/H). 320×192 = 61 440 px vs 256² = 65 536 → **HW 541 → 491 ms** (−9 %, y −39 % desde V189). QEMU sobre `bus.jpg`@320×192: 4× person + bus (correcto). Golden recapturado de cámara al nuevo formato (`clock` 56 % — más pequeño en el frame que con el recorte; la huella `[ABS]` es el chequeo fuerte). `test_image.bin` 786432→737280 B, `d2m_data.bin` 13 345 344 B md5 `01d902a0…` (recopiar a SD). Tradeoff: objetos más pequeños → menos confianza; a cambio, sin ceguera lateral. Base para el A/B de modelos (v5n@320, v11n) sobre la misma imagen. |
| **V193** | **Winograd F(2×2,3×3) — implementado, validado, CERRADO (verdicto negativo).** Objetivo: el fast-path K=3/s1/p1 de 8 canales (`conv2d_partial_8ch` P8) ya está cerca del piso NEON del A53 (V191); Winograd corta el producto interno de 9 taps a 16 productos elementwise acumulados sobre C_in (9/4 = 2.25× menos FMA por píxel), a cambio de una transformada lineal barata (solo sumas/restas + un ×0.5 por fila) del filtro 3×3 y del tile de entrada 4×4. Matemática validada standalone (`winograd_explicit.cpp` en el scratchpad de sesión, error abs máx ~2e-6 vs conv directa) antes de tocar NEON. Implementación en `bsp/multicore.cpp` (`conv2d_winograd_tile_8ch` + 3 helpers `winograd_{weight,input,output}_transform`), enganchada en el fast-path de `conv2d_partial_8ch` para el rectángulo 2×2-tile-alineado de la región segura; el resto (anillo de 1 px de padding, tiles impares en el borde) cae al camino P8/genérico existente sin tocar. Correcto en HW real ambos intentos: detecciones + huella `[ABS]` PASS 16/16. **Pero es más lento**: 492 → 521-523 ms (+6 %), consistente en 3 corridas, con y sin vectorizar la transformada de entrada (segundo intento: reemplazar 16 cargas escalares independientes por 4 `vld1q_f32` + extracción de carriles en registro — cero diferencia medida). Causa raíz: el acumulador `M` (16 taps × hasta 8 tiles × 2 mitades lo/hi = 256 `float32x4_t`) excede muy por encima los 32 registros NEON físicos del A53 — casi cada acumulación es load+FMA+store (1 FMA por toque), contra las **9** FMA por toque del kernel P8 existente (diseño weight-stationary: 9 taps hacia 1 acumulador cargado/guardado una vez). La ganancia aritmética (9/4× menos FMA) es menor que la pérdida en la proporción load/store-a-FMA (9× peor). Una ganancia real necesitaría una reestructuración estilo GEMM (tiles en los carriles vectoriales, no los 8 canales de salida) — reescritura mayor, no intentada. Detectado y corregido en el camino un bug de medición propio: dejar el código Winograd como rama muerta en tiempo de ejecución (`if (false && …)`) dentro de `conv2d_partial_8ch` seguía costando ~12 ms/frame por el tamaño del stack frame de esa rama en cada una de sus muchas invocaciones por frame — solucionado moviendo la función completa detrás de `#ifndef D2M_NO_WINOGRAD` para que no exista en absoluto en el binario por defecto. Mantenido en el árbol, compilable, detrás de `WINOGRAD=0` (default) / `make WINOGRAD=1`, como punto de partida documentado si alguien retoma la reescritura GEMM. Veredicto: cerrado, igual que INT8. |
| **V194** | **Instrumentación del SoC (`[SOC]`) — el "throttling térmico" no existía.** `soc_status_read/report` en `bsp/mailbox.cpp` (declarados en `bsp.h`): un solo round-trip del canal de propiedades con tres tags — `0x00030046` get_throttled, `0x00030047` get_clock_rate_measured (id 3 = ARM), `0x00030006` get_temperature. Imprime `[SOC] arm=1000MHz temp=45.0C thr=0x00000000` en el boot y, tras `[T]`, en cada frame de inferencia bajo `SERIAL_BOOT` (cada 32 en el build normal, para no gastar UART en vivo). La MMU ya está activa, así que el buffer `mbox` (BSS cacheable) se limpia con `flush_to_ram` antes y después del `mbox_call`; los tags no soportados se detectan por el bit 31 del word de request/response (QEMU responde los tres con valores stub: `arm=0MHz temp=25.0C`). **Resultado — dos hipótesis falsadas:** (1) la deriva 492→504 ms **no es throttling**: `arm=1000MHz` y `thr=0x00000000` (ni siquiera los bits 16-19 de "ocurrió alguna vez") en corridas a 43, 46, 50 y 51.5 °C; (2) tampoco es la instrumentación `C2F_PROF` — A/B con `C2F_PROF=0` (rebuild forzado; el Makefile no rastrea cambios de flags, el primer intento midió el binario viejo) da **504 ms exactos**. Lo que sí queda establecido es que **el banco es reproducible a ±0 ms**: 504/504/504 en tres corridas seguidas con el SoC subiendo 8 °C, así que un A/B de 2-3 ms es detectable y no hace falta promediar. Nueva línea base con condiciones registradas: **504 ms @ 1000 MHz, sin throttle** (el 492 de V192 se midió en condiciones que nadie anotó — que es exactamente el problema que esto cierra). Corrección de dato: `IMPROVEMENTS.md` daba `0x00030006` como el tag de throttle; ese es *get_temperature*, el de throttle es `0x00030046`. |

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
